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

#include "TinyCanInterface.h"
#include "TinyCanDriver.h"

#include <chrono>

#include <QCanBus>
#include <QCanBusDevice>
#include <QCanBusFrame>
#include <QMutexLocker>

#include "core/Backend.h"
#include "core/BusMessage.h"
#include "core/MeasurementInterface.h"

TinyCanInterface::TinyCanInterface(TinyCanDriver *driver, QString deviceName, QString description)
  : BusInterface(reinterpret_cast<CanDriver*>(driver)),
    _deviceName(deviceName),
    _name(description),
    _device(nullptr),
    _bitrate(500000),
    _listenOnly(false)
{
    memset(&_stats, 0, sizeof(_stats));
    memset(&_offset_stats, 0, sizeof(_offset_stats));
}

TinyCanInterface::~TinyCanInterface()
{
    if (isOpen()) {
        close();
    }
}

QString TinyCanInterface::getName() const
{
    return _name;
}

void TinyCanInterface::setName(QString name)
{
    _name = name;
}

QList<CanTiming> TinyCanInterface::getAvailableBitrates()
{
    QList<CanTiming> retval;
    QList<unsigned> bitrates({10000, 20000, 50000, 83333, 100000, 125000,
                               250000, 500000, 800000, 1000000});
    unsigned i = 0;
    for (unsigned br : bitrates) {
        retval << CanTiming(i++, br, 0, 875);
    }
    return retval;
}

void TinyCanInterface::applyConfig(const MeasurementInterface &mi)
{
    if (!mi.doConfigure()) {
        log_info(QString("TinyCanInterface %1: not managed by cangaroo, skipping configuration")
                     .arg(_name));
        return;
    }
    _bitrate    = mi.bitrate();
    _listenOnly = mi.isListenOnlyMode();
    log_info(QString("TinyCanInterface %1: configuration stored, bitrate=%2")
                 .arg(_name).arg(_bitrate));
}

unsigned TinyCanInterface::getBitrate()
{
    return _bitrate;
}

uint32_t TinyCanInterface::getCapabilities()
{
    return BusInterface::capability_listen_only;
}

void TinyCanInterface::open()
{
    QString errorString;
    _device = QCanBus::instance()->createDevice(
        QStringLiteral("tinycan"), _deviceName, &errorString);

    if (!_device) {
        log_error(QString("TinyCanInterface %1: createDevice failed: %2")
                      .arg(_name, errorString));
        return;
    }

    _device->setConfigurationParameter(
        QCanBusDevice::BitRateKey, _bitrate);

    if (_listenOnly) {
        _device->setConfigurationParameter(
            QCanBusDevice::ReceiveOwnKey, false);
    }

    if (!_device->connectDevice()) {
        log_error(QString("TinyCanInterface %1: connectDevice failed: %2")
                      .arg(_name, _device->errorString()));
        delete _device;
        _device = nullptr;
        return;
    }

    _openEpochUs = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    _isOpen.store(true);
    log_info(QString("TinyCanInterface %1: opened").arg(_name));
}

bool TinyCanInterface::isOpen()
{
    return _isOpen.load();
}

void TinyCanInterface::close()
{
    _isOpen.store(false);
    if (_device) {
        _device->disconnectDevice();
        delete _device;
        _device = nullptr;
    }
    log_info(QString("TinyCanInterface %1: closed").arg(_name));
}

void TinyCanInterface::sendMessage(const BusMessage &msg)
{
    if (!isOpen()) {
        log_error(QString("TinyCanInterface %1: cannot send, interface not open").arg(_name));
        return;
    }

    QCanBusFrame frame;
    frame.setFrameId(msg.getId());
    frame.setExtendedFrameFormat(msg.isExtended());

    if (msg.isRTR()) {
        frame.setFrameType(QCanBusFrame::RemoteRequestFrame);
    } else {
        frame.setFrameType(QCanBusFrame::DataFrame);
        uint8_t len = (msg.getLength() > 8) ? 8 : msg.getLength();
        QByteArray payload(len, 0);
        for (int i = 0; i < len; i++) {
            payload[i] = static_cast<char>(msg.getByte(i));
        }
        frame.setPayload(payload);
    }

    if (!_device->writeFrame(frame)) {
        log_error(QString("TinyCanInterface %1: writeFrame failed: %2")
                      .arg(_name, _device->errorString()));
        _stats.tx_errors++;
    } else {
        _stats.tx_count++;
        addFrameBits(msg);

        BusMessage txMsg = msg;
        txMsg.setRX(false);
        auto now = std::chrono::system_clock::now().time_since_epoch();
        txMsg.setTimestamp_us(std::chrono::duration_cast<std::chrono::microseconds>(now).count());
        QMutexLocker lock(&_txMutex);
        _txMsgList.append(txMsg);
    }
}

bool TinyCanInterface::readMessage(QList<BusMessage> &msglist, unsigned int timeout_ms)
{
    if (!isOpen()) {
        return false;
    }

    // Enqueue tx messages
    {
        QMutexLocker lock(&_txMutex);
        msglist.append(_txMsgList);
        _txMsgList.clear();
    }
    bool hasTx = !msglist.isEmpty();
    if (hasTx)
    {
        timeout_ms = 1;
    }

    if (!_device->framesAvailable()) {
        if (!_device->waitForFramesReceived(static_cast<int>(timeout_ms))) {
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
    msg.setErrorFrame(frame.frameType() == QCanBusFrame::ErrorFrame);
    msg.setInterfaceId(getId());

    const QCanBusFrame::TimeStamp ts = frame.timeStamp();
    msg.setTimestamp_us(_openEpochUs
        + static_cast<int64_t>(ts.seconds()) * 1000000LL
        + static_cast<int64_t>(ts.microSeconds()));

    const QByteArray payload = frame.payload();
    uint8_t len = static_cast<uint8_t>(qMin(payload.size(), 8));
    msg.setLength(len);
    for (int i = 0; i < len; i++) {
        msg.setByte(i, static_cast<uint8_t>(payload.at(i)));
    }

    msglist.append(msg);
    _stats.rx_count++;
    addFrameBits(msg);
    return true;
}

bool TinyCanInterface::updateStatistics()
{
    return isOpen();
}

void TinyCanInterface::resetStatistics()
{
    _offset_stats = _stats;
    BusInterface::resetStatistics();
}

uint32_t TinyCanInterface::getState()
{
    if (!isOpen()) {
        return state_stopped;
    }

    switch (_device->busStatus()) {
        case QCanBusDevice::CanBusStatus::Good:    return state_ok;
        case QCanBusDevice::CanBusStatus::Warning: return state_warning;
        case QCanBusDevice::CanBusStatus::Error:   return state_passive;
        case QCanBusDevice::CanBusStatus::BusOff:  return state_bus_off;
        default:                                   return state_unknown;
    }
}

int TinyCanInterface::getNumRxFrames()   { return static_cast<int>(_stats.rx_count    - _offset_stats.rx_count);    }
int TinyCanInterface::getNumRxErrors()   { return _stats.rx_errors   - _offset_stats.rx_errors;    }
int TinyCanInterface::getNumRxOverruns() { return static_cast<int>(_stats.rx_overruns - _offset_stats.rx_overruns); }
int TinyCanInterface::getNumTxFrames()   { return static_cast<int>(_stats.tx_count    - _offset_stats.tx_count);    }
int TinyCanInterface::getNumTxErrors()   { return _stats.tx_errors   - _offset_stats.tx_errors;    }
int TinyCanInterface::getNumTxDropped()  { return static_cast<int>(_stats.tx_dropped  - _offset_stats.tx_dropped);  }

QString TinyCanInterface::getDeviceName() const
{
    return _deviceName;
}
