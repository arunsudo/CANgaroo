/*

  Copyright (c) 2016 Hubert Denkmair <hubert@denkmair.de>

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
#include "api/candle_defs.h"
#include "CandleApiDriver.h"
#include "api/candle.h"

#include "CandleApiInterface.h"
#include "driver/GenericCanSetupPage.h"


CandleApiDriver::CandleApiDriver(Backend &backend)
  : CanDriver(backend),
    setupPage(new GenericCanSetupPage(0))
{
    QObject::connect(&backend, &Backend::onSetupDialogCreated, setupPage, &GenericCanSetupPage::onSetupDialogCreated);
}

QString CandleApiDriver::getName() const
{
    return "CandleAPI";
}

bool CandleApiDriver::update()
{
    // Destroy existing interfaces first, then release shared devices.
    // Interfaces hold shared_ptr refs to the devices, so deleting them first
    // ensures the devices are freed in the correct order.
    deleteAllInterfaces();
    _devices.clear();

    candle_list_handle clist;
    if (!candle_list_scan(&clist)) {
        return true;
    }

    uint8_t num_devices = 0;
    if (!candle_list_length(clist, &num_devices)) {
        candle_list_free(clist);
        return true;
    }

    for (uint8_t i = 0; i < num_devices; i++) {
        candle_handle dev;
        if (!candle_dev_get(clist, i, &dev)) {
            continue;
        }

        // Open temporarily to read channel count and per-channel capabilities.
        if (!candle_dev_open(dev)) {
            candle_dev_free(dev);
            continue;
        }

        uint8_t num_channels = 0;
        if (!candle_channel_count(dev, &num_channels) || num_channels == 0) {
            candle_dev_close(dev);
            candle_dev_free(dev);
            continue;
        }

        const std::wstring devPath(candle_dev_get_path(dev));

        // Create a single shared handle for all channels on this device.
        // candle_dev_get() allocates a fresh candle_device_t; we pre-populate
        // its capability fields so getAvailableBitrates() works before open().
        candle_handle shared_handle;
        candle_dev_get(clist, i, &shared_handle);
        {
            candle_device_t *src = static_cast<candle_device_t*>(dev);
            candle_device_t *dst = static_cast<candle_device_t*>(shared_handle);
            wcscpy(dst->path, src->path);
            dst->state   = src->state;
            dst->dconf   = src->dconf;
            dst->bt_const = src->bt_const;
            memcpy(dst->ch_caps, src->ch_caps, sizeof(src->ch_caps));
        }

        candle_dev_close(dev);
        candle_dev_free(dev);

        auto sharedDev = std::make_shared<CandleSharedDevice>();
        sharedDev->handle = shared_handle;
        _devices[devPath] = sharedDev;

        // One BusInterface per channel, all sharing the same physical device.
        for (uint8_t ch = 0; ch < num_channels; ch++) {
            addInterface(new CandleApiInterface(this, sharedDev, ch));
        }
    }

    candle_list_free(clist);
    return true;
}
