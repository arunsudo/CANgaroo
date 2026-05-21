/*

  Copyright (c) 2026 Schildkroet

  This file is part of cangaroo.

  cangaroo is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  cangaroo is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with cangaroo.  If not, see <http://www.gnu.org/licenses/>.

*/

#include "VectorInterface.h"
#include "VectorDriver.h"

#include <chrono>

#include <QCanBus>
#include <QCanBusDevice>
#include <QCanBusFrame>
#include <QMutexLocker>

#include "core/Backend.h"
#include "core/BusMessage.h"
#include "core/MeasurementInterface.h"

VectorInterface::VectorInterface(VectorDriver *driver, QString deviceName, QString description)
  : BusInterface(reinterpret_cast<CanDriver*>(driver)),
    _deviceName(deviceName),
    _name(description),
    _device(nullptr),
    _bitrate(500000),
    _fdBitrate(2000000),
    _listenOnly(false),
    _isCanFD(false)
{
    memset(&_stats, 0, sizeof(_stats));
    memset(&_offset_stats, 0, sizeof(_offset_stats));
}

VectorInterface::~VectorInterface()
{
    if (isOpen()) {
        close();
    }
}

QString VectorInterface::getName() const
{
    return _name;
}

void VectorInterface::setName(QString name)
{
    _name = name;
}

QList<CanTiming> VectorInterface::getAvailableBitrates()
{
    QList<CanTiming> retval;

    const QList<unsigned> nominalRates({10000, 20000, 50000, 83333, 100000,
                                         125000, 250000, 500000, 800000, 1000000});
    const QList<unsigned> fdDataRates({500000, 1000000, 2000000, 4000000,
                                        5000000, 8000000, 10000000});

    unsigned i = 0;
    for (unsigned br : nominalRates) {
        // Classic CAN entry
        retval << CanTiming(i++, br, 0, 875);
        // CAN FD entries: pair with each data rate that is >= nominal rate
        for (unsigned fdbr : fdDataRates) {
            if (fdbr >= br) {
                retval << CanTiming(i++, br, fdbr, 875, 800);
            }
        }
    }
    return retval;
}

void VectorInterface::applyConfig(const MeasurementInterface &mi)
{
    if (!mi.doConfigure()) {
        log_info(QString("VectorInterface %1: not managed by cangaroo, skipping configuration")
                     .arg(_name));
        return;
    }
    _bitrate    = mi.bitrate();
    _listenOnly = mi.isListenOnlyMode();
    _isCanFD    = mi.isCanFD();
    _fdBitrate  = mi.fdBitrate();
    log_info(QString("VectorInterface %1: configuration stored, bitrate=%2, canfd=%3, fdBitrate=%4")
                 .arg(_name).arg(_bitrate).arg(_isCanFD).arg(_fdBitrate));
}

unsigned VectorInterface::getBitrate()
{
    return _bitrate;
}

uint32_t VectorInterface::getCapabilities()
{
    return BusInterface::capability_listen_only
         | BusInterface::capability_canfd;
}

void VectorInterface::open()
{
    QString errorString;
    _device = QCanBus::instance()->createDevice(
        QStringLiteral("vectorcan"), _deviceName, &errorString);

    if (!_device) {
        log_error(QString("VectorInterface %1: createDevice failed: %2")
                      .arg(_name, errorString));
        return;
    }

    _device->setConfigurationParameter(QCanBusDevice::BitRateKey, _bitrate);

    if (_isCanFD) {
        _device->setConfigurationParameter(QCanBusDevice::CanFdKey, true);
        _device->setConfigurationParameter(QCanBusDevice::DataBitRateKey, _fdBitrate);
    }

    if (_listenOnly) {
        // LoopbackKey=false prevents the device from ACKing on the bus (listen-only).
        // ReceiveOwnKey controls echoing of own TX frames — keep it off in listen-only.
        _device->setConfigurationParameter(QCanBusDevice::LoopbackKey, false);
        _device->setConfigurationParameter(QCanBusDevice::ReceiveOwnKey, false);
    }

    if (!_device->connectDevice()) {
        log_error(QString("VectorInterface %1: connectDevice failed: %2")
                      .arg(_name, _device->errorString()));
        delete _device;
        _device = nullptr;
        return;
    }

    _openEpochUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    _isOpen.store(true);
    log_info(QString("VectorInterface %1: opened").arg(_name));
}

bool VectorInterface::isOpen()
{
    return _isOpen.load();
}

void VectorInterface::close()
{
    _isOpen.store(false);
    if (_device) {
        _device->disconnectDevice();
        delete _device;
        _device = nullptr;
    }
    log_info(QString("VectorInterface %1: closed").arg(_name));
}

void VectorInterface::sendMessage(const BusMessage &msg)
{
    if (!isOpen()) {
        log_error(QString("VectorInterface %1: cannot send, interface not open").arg(_name));
        return;
    }
    // Queue only — actual writeFrame() happens inside readMessage(), which runs
    // on the same thread as open() and owns the QCanBusDevice.
    QMutexLocker lock(&_txMutex);
    _txMsgList.append(msg);
}

bool VectorInterface::readMessage(QList<BusMessage> &msglist, unsigned int timeout_ms)
{
    if (!isOpen()) {
        return false;
    }

    // Drain the TX queue. writeFrame() must be called from the thread that owns
    // _device (the CanListener thread that also called open()).
    {
        QMutexLocker lock(&_txMutex);
        for (const BusMessage &qMsg : std::as_const(_txMsgList)) {
            QCanBusFrame frame;
            frame.setFrameId(qMsg.getId());
            frame.setExtendedFrameFormat(qMsg.isExtended());

            if (qMsg.isRTR()) {
                frame.setFrameType(QCanBusFrame::RemoteRequestFrame);
            } else {
                frame.setFrameType(QCanBusFrame::DataFrame);
                const uint8_t maxLen = qMsg.isFD() ? 64 : 8;
                const uint8_t len = qMin(qMsg.getLength(), maxLen);
                QByteArray payload(len, 0);
                for (int i = 0; i < len; i++) {
                    payload[i] = static_cast<char>(qMsg.getByte(i));
                }
                frame.setPayload(payload);
                if (qMsg.isFD()) {
                    frame.setFlexibleDataRateFormat(true);
                    frame.setBitrateSwitch(qMsg.isBRS());
                }
            }

            if (!_device->writeFrame(frame)) {
                log_error(QString("VectorInterface %1: writeFrame failed: %2")
                              .arg(_name, _device->errorString()));
                _stats.tx_errors++;
            } else {
                _stats.tx_count++;
                addFrameBits(qMsg);

                BusMessage txMsg = qMsg;
                txMsg.setRX(false);
                auto now = std::chrono::system_clock::now().time_since_epoch();
                txMsg.setTimestamp_us(
                    std::chrono::duration_cast<std::chrono::microseconds>(now).count());
                msglist.append(txMsg);
            }
        }
        _txMsgList.clear();
    }

    const bool hasTx = !msglist.isEmpty();
    const int waitMs = hasTx ? 1 : static_cast<int>(timeout_ms);

    if (!_device->framesAvailable()) {
        if (!_device->waitForFramesReceived(waitMs)) {
            return hasTx;
        }
    }

    const QCanBusFrame frame = _device->readFrame();
    if (!frame.isValid()) {
        return hasTx;
    }

    if (frame.frameType() == QCanBusFrame::ErrorFrame) {
        _stats.rx_errors++;
        return hasTx;
    }

    BusMessage msg;
    msg.setId(frame.frameId());
    msg.setExtended(frame.hasExtendedFrameFormat());
    msg.setRTR(frame.frameType() == QCanBusFrame::RemoteRequestFrame);
    msg.setErrorFrame(false);
    msg.setInterfaceId(getId());

    const bool isFD = frame.hasFlexibleDataRateFormat();
    msg.setFD(isFD);
    msg.setBRS(isFD && frame.hasBitrateSwitch());

    const QCanBusFrame::TimeStamp ts = frame.timeStamp();
    msg.setTimestamp_us(_openEpochUs
        + static_cast<int64_t>(ts.seconds()) * 1000000LL
        + static_cast<int64_t>(ts.microSeconds()));

    const QByteArray payload = frame.payload();
    const int maxLen = isFD ? 64 : 8;
    uint8_t len = static_cast<uint8_t>(qMin(payload.size(), maxLen));
    msg.setLength(len);
    for (int i = 0; i < len; i++) {
        msg.setByte(i, static_cast<uint8_t>(payload.at(i)));
    }

    msglist.append(msg);
    _stats.rx_count++;
    addFrameBits(msg);
    return true;
}

bool VectorInterface::updateStatistics()
{
    return isOpen();
}

void VectorInterface::resetStatistics()
{
    _offset_stats = _stats;
    BusInterface::resetStatistics();
}

uint32_t VectorInterface::getState()
{
    if (!isOpen()) {
        return state_stopped;
    }

    switch (_device->busStatus()) {
        case QCanBusDevice::CanBusStatus::Good:    return state_ok;
        case QCanBusDevice::CanBusStatus::Warning: return state_warning;
        case QCanBusDevice::CanBusStatus::Error:   return state_passive;
        case QCanBusDevice::CanBusStatus::BusOff:  return state_bus_off;
        case QCanBusDevice::CanBusStatus::Unknown:
        default:
            // vectorcan plugin may not implement busStatus(); treat a connected
            // device as good rather than showing a misleading unknown state.
            return state_ok;
    }
}

int VectorInterface::getNumRxFrames()   { return (int)(_stats.rx_count    - _offset_stats.rx_count);    }
int VectorInterface::getNumRxErrors()   { return       _stats.rx_errors   - _offset_stats.rx_errors;    }
int VectorInterface::getNumRxOverruns() { return (int)(_stats.rx_overruns - _offset_stats.rx_overruns); }
int VectorInterface::getNumTxFrames()   { return (int)(_stats.tx_count    - _offset_stats.tx_count);    }
int VectorInterface::getNumTxErrors()   { return       _stats.tx_errors   - _offset_stats.tx_errors;    }
int VectorInterface::getNumTxDropped()  { return (int)(_stats.tx_dropped  - _offset_stats.tx_dropped);  }

QString VectorInterface::getDeviceName() const
{
    return _deviceName;
}
