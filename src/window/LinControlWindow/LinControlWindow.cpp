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

#include "LinControlWindow.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>

#include "core/Backend.h"
#include "core/MeasurementSetup.h"
#include "core/MeasurementNetwork.h"
#include "core/MeasurementInterface.h"
#include "driver/BusInterface.h"
#include "core/BusMessage.h"

LinControlWindow::LinControlWindow(QWidget *parent, Backend &backend)
    : ConfigurableWidget(parent)
    , _backend(backend)
{
    auto *outerLayout = new QVBoxLayout(this);
    outerLayout->setContentsMargins(4, 4, 4, 4);

    auto *scrollArea = new QScrollArea(this);
    scrollArea->setWidgetResizable(true);
    scrollArea->setFrameShape(QFrame::NoFrame);

    _rowContainer = new QWidget(scrollArea);
    _rowLayout = new QVBoxLayout(_rowContainer);
    _rowLayout->setAlignment(Qt::AlignTop);
    _rowLayout->setSpacing(4);
    _rowLayout->setContentsMargins(0, 0, 0, 0);

    _placeholder = new QLabel(tr("No LIN interfaces configured"), _rowContainer);
    _placeholder->setAlignment(Qt::AlignCenter);
    _placeholder->setEnabled(false);
    _rowLayout->addWidget(_placeholder);

    scrollArea->setWidget(_rowContainer);
    outerLayout->addWidget(scrollArea);

    connect(&_backend, &Backend::beginMeasurement, this, &LinControlWindow::rebuildRows);
    connect(&_backend, &Backend::endMeasurement,   this, &LinControlWindow::clearRows);

    if (_backend.isMeasurementRunning())
        rebuildRows();
}

LinControlWindow::~LinControlWindow() = default;

void LinControlWindow::retranslateUi()
{
    _placeholder->setText(tr("No LIN interfaces configured"));
}

void LinControlWindow::rebuildRows()
{
    clearRows();

    bool anyLin = false;

    for (MeasurementNetwork *network : _backend.getSetup().getNetworks())
    {
        for (MeasurementInterface *mi : network->interfaces())
        {
            if (mi->busType() != BusType::LIN)
                continue;

            BusInterface *iface = _backend.getInterfaceById(mi->busInterface());
            if (!iface)
                continue;

            anyLin = true;

            auto *row = new QWidget(_rowContainer);
            auto *rowLayout = new QHBoxLayout(row);
            rowLayout->setContentsMargins(2, 2, 2, 2);

            auto *label = new QLabel(iface->getName(), row);
            label->setMinimumWidth(120);

            auto *sleepBtn  = new QPushButton(tr("Sleep"),  row);
            auto *wakeupBtn = new QPushButton(tr("Wakeup"), row);

            rowLayout->addWidget(label);
            rowLayout->addStretch();
            rowLayout->addWidget(sleepBtn);
            rowLayout->addWidget(wakeupBtn);

            _rowLayout->addWidget(row);

            connect(sleepBtn,  &QPushButton::clicked, this, [iface]() { iface->sendLinSleepWakeup(false); });
            connect(wakeupBtn, &QPushButton::clicked, this, [iface]() { iface->sendLinSleepWakeup(true);  });
        }
    }

    _placeholder->setVisible(!anyLin);
}

void LinControlWindow::clearRows()
{
    QLayoutItem *item;
    while ((item = _rowLayout->takeAt(0)) != nullptr)
    {
        if (QWidget *w = item->widget(); w && w != _placeholder)
            w->deleteLater();
        delete item;
    }

    _rowLayout->addWidget(_placeholder);
    _placeholder->setVisible(true);
}
