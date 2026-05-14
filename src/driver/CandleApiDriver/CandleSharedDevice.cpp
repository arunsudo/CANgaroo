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

#include "CandleSharedDevice.h"

#include <QMutexLocker>
#include <QDeadlineTimer>

CandleSharedDevice::~CandleSharedDevice()
{
    stopReader();
    if (handle) {
        if (openCount > 0) {
            candle_dev_close(handle);
        }
        candle_dev_free(handle);
    }
}

void CandleSharedDevice::startReader()
{
    readerRunning.store(true, std::memory_order_relaxed);
    readerThread = std::thread([this]()
    {
        while (readerRunning.load(std::memory_order_relaxed)) {
            candle_fd_frame_t frame;
            // Short timeout so the loop checks readerRunning frequently.
            if (!candle_fd_frame_read(handle, &frame, 50)) {
                continue;
            }
            if (candle_fd_frame_type(&frame) != CANDLE_FRAMETYPE_RECEIVE) {
                continue;
            }
            const uint8_t ch = frame.channel;
            if (ch < MAX_CHANNELS) {
                QMutexLocker lock(&queueMutex);
                rxQueues[ch].append(frame);
                queueCond.wakeAll();
            }
        }
    });
}

void CandleSharedDevice::stopReader()
{
    readerRunning.store(false, std::memory_order_relaxed);
    if (readerThread.joinable()) {
        readerThread.join();
    }
    QMutexLocker lock(&queueMutex);
    for (auto &q : rxQueues) {
        q.clear();
    }
}

bool CandleSharedDevice::readFrame(uint8_t channel, candle_fd_frame_t &frame, unsigned timeout_ms)
{
    QMutexLocker lock(&queueMutex);
    const QDeadlineTimer deadline(timeout_ms);
    while (rxQueues[channel].isEmpty()) {
        if (!queueCond.wait(&queueMutex, deadline)) {
            return false;
        }
    }
    frame = rxQueues[channel].takeFirst();
    return true;
}
