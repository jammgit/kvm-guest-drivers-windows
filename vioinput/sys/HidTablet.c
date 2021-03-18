﻿/*
 * Tablet specific HID functionality
 *
 * Copyright (c) 2016-2017 Red Hat, Inc.
 *
 * Author(s):
 *  Ladi Prosek <lprosek@redhat.com>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met :
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and / or other materials provided with the distribution.
 * 3. Neither the names of the copyright holders nor the names of their contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.IN NO EVENT SHALL THE COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "precomp.h"
#include "vioinput.h"
#include "Hid.h"

#if defined(EVENT_TRACING)
#include "HidTablet.tmh"
#endif

 // Defined in drivers/hid/hid-multitouch.c
#define MT_DEFAULT_MAXCONTACT   10
#define MT_MAX_MAXCONTACT       250
#define MT_DIRTY_GROUPS ((MT_MAX_MAXCONTACT + MT_MAX_MAXCONTACT%MT_MAX_MAXCONTACT)/(sizeof(ULONG)*8))

#pragma pack(push,1)
typedef struct _tagInputClassTabletFeatureMaxContact
{
    UCHAR uReportID;
    UCHAR uMaxContacts;
}INPUT_CLASS_TABLET_FEATURE_MAX_CONTACT, *PINPUT_CLASS_TABLET_FEATURE_MAX_CONTACT;

typedef struct _tagInputClassTabletSlot
{
    UCHAR uFlags;
    USHORT uContactID;
    USHORT uAxisX;
    USHORT uAxisY;
    USHORT uWidth;
    USHORT uHeight;
} INPUT_CLASS_TABLET_SLOT, *PINPUT_CLASS_TABLET_SLOT;
#pragma pack(pop)

typedef struct _tagInputClassTabletTrackingID
{
    LONG uID;
    BOOLEAN bPendingDel;
} INPUT_CLASS_TABLET_TRACKING_ID, *PINPUT_CLASS_TABLET_TRACKING_ID;

typedef struct _tagInputClassTablet
{
    INPUT_CLASS_COMMON Common;
    BOOLEAN bMT;
    BOOLEAN bIdentifiableMT;
    BOOLEAN bMscTs;
    ULONG uMaxContacts;
    ULONG uLastMTSlot;
    // VIOInputAlloc(sizeof(INPUT_CLASS_TABLET_SLOT) * pTabletDesc->uMaxContacts);
    PINPUT_CLASS_TABLET_SLOT pContactStat;
    // VIOInputAlloc(sizeof(INPUT_CLASS_TABLET_TRACKING_ID) * pTabletDesc->uMaxContacts);
    PINPUT_CLASS_TABLET_TRACKING_ID pTrackingID;

    /*
     * HID Tablet report layout:
     * Total size in bytes: 1 + 7 * numContacts + 1 + 4
     * +---------------+-+-+-+-+-+---------------+----------+------------+
     * | Byte Offset   |7|6|5|4|3|     2         |    1     |     0      |
     * +---------------+-+-+-+-+-+---------------+----------+------------+
     * | 0             |                    Report ID                    |
     * | i*7+1         |   Pad   | Barrel Switch | In-range | Tip Switch |
     * | i*7+[2,3]     |                    Contact ID                   |
     * | i*7+[4,5]     |                      x-axis                     |
     * | i*7+[6,7]     |                      y-axis                     |
     * | (i+1)*7+[1,7] |                   Contact i+1                   |
     * | (i+2)*7+[1,7] |                   Contact i+2                   |
     * | ...           |                                                 |
     * | (n-1)*7+[1,7] |                   Contact n-1                   |
     * | n*7+1         |                  Contact Count                  |
     * | n*7+[2,5]     |                    Scan Time                    |
     * +---------------+-+-+-+-+-+------------+----------+---------------+
     */
} INPUT_CLASS_TABLET, *PINPUT_CLASS_TABLET;

static NTSTATUS
HIDTabletGetFeature(
    PINPUT_CLASS_COMMON pClass,
    PHID_XFER_PACKET pFeaturePkt)
{
    PINPUT_CLASS_TABLET pTabletDesc = (PINPUT_CLASS_TABLET)pClass;
    UCHAR uReportID = *(PUCHAR)pFeaturePkt->reportBuffer;
    NTSTATUS status = STATUS_SUCCESS;

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "--> %s\n", __FUNCTION__);

    switch (uReportID)
    {
    case REPORTID_FEATURE_TABLET_MAX_COUNT:
        if (pFeaturePkt->reportBufferLen >= sizeof(INPUT_CLASS_TABLET_FEATURE_MAX_CONTACT))
        {
            PINPUT_CLASS_TABLET_FEATURE_MAX_CONTACT pFtrReport = (PINPUT_CLASS_TABLET_FEATURE_MAX_CONTACT)pFeaturePkt->reportBuffer;

            pFtrReport->uMaxContacts = (UCHAR)pTabletDesc->uMaxContacts;
        }
        else
        {
            status = STATUS_BUFFER_TOO_SMALL;
        }
        break;
    default:
        status = STATUS_INVALID_PARAMETER;
        break;
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "<-- %s\n", __FUNCTION__);

    return status;
}

static WDFDEVICE hDevice = NULL;
KSPIN_LOCK lastDataLock;
UCHAR data[HID_REPORT_DATA_OFFSET + sizeof(INPUT_CLASS_TABLET_SLOT)*MT_MAX_MAXCONTACT];
ULONG dataLen = 0;
volatile BOOLEAN pressed = FALSE;

static BOOLEAN                  timerCreateSuccess = FALSE;
static BOOLEAN                  timerStartSuccess = FALSE;
static WDF_TIMER_CONFIG         timerConfig;
static WDF_OBJECT_ATTRIBUTES    timerAttributes;
static WDFTIMER                 hTimer;

void EvtWdfTimer(WDFTIMER Timer)
{
    NTSTATUS status;
    WDFREQUEST request;
    WDFDEVICE device = WdfTimerGetParentObject(Timer);
    PINPUT_DEVICE pContext = GetDeviceContext(device);

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "--> %s\n", __FUNCTION__);

    KIRQL irql;
    KeAcquireSpinLock(&lastDataLock, &irql);
    if (pressed && dataLen > 0)
    {
        status = WdfIoQueueRetrieveNextRequest(pContext->HidQueue, &request);
        if (NT_SUCCESS(status))
        {
            TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "WdfIoQueueRetrieveNextRequest success\n");

            status = RequestCopyFromBuffer(
                request,
                data,
                dataLen);
            KeReleaseSpinLock(&lastDataLock, irql);

            WdfRequestComplete(request, status);
            if (NT_SUCCESS(status))
            {
                TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "RequestCopyFromBuffer success\n");
            }
            else
            {
                TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "RequestCopyFromBuffer failed\n");
            }
        }
        else
        {
            TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "get next status:0x%x\n", status);
            KeReleaseSpinLock(&lastDataLock, irql);
        }
    }
    else
    {
        TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "press:%d datalen:%d\n", pressed, dataLen);
        KeReleaseSpinLock(&lastDataLock, irql);
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "<-- %s\n", __FUNCTION__);
}

void HIDInitialzeData(WDFDEVICE device)
{
    KeInitializeSpinLock(&lastDataLock);

    hDevice = device;

    WDF_TIMER_CONFIG_INIT(
        &timerConfig,
        EvtWdfTimer
    );
    timerConfig.Period = 50;
    timerConfig.AutomaticSerialization = TRUE;

    WDF_OBJECT_ATTRIBUTES_INIT(&timerAttributes);
    timerAttributes.ParentObject = device;

    NTSTATUS status = WdfTimerCreate(
        &timerConfig,
        &timerAttributes,
        &hTimer
    );
    if (!NT_SUCCESS(status))
    {
        TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "--> %s create failed, 0x%x\n", __FUNCTION__, status);
    }
    else
    {
        TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "--> %s create success\n", __FUNCTION__);
        timerCreateSuccess = TRUE;
        timerStartSuccess = WdfTimerStart(hTimer, WDF_REL_TIMEOUT_IN_MS(50));
        if (timerStartSuccess)
        {
            TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "--> %s start success\n", __FUNCTION__);
        }
        else
        {
            TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "--> %s start failed\n", __FUNCTION__);
        }
    }
}

void HIDReleaseData()
{
    // WdfTimerStop(hTimer, TRUE);
}

static NTSTATUS
HIDTabletEventToCollect(
    PINPUT_CLASS_COMMON pClass,
    PVIRTIO_INPUT_EVENT pEvent)
{
    PINPUT_CLASS_TABLET pTabletDesc = (PINPUT_CLASS_TABLET)pClass;
    PUCHAR pReport = pClass->pHidReport;
    PINPUT_CLASS_TABLET_SLOT pReportSlot;
    ULONG uNumContacts;

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "--> %s\n", __FUNCTION__);

    switch (pEvent->type)
    {
    case EV_SYN:
        switch (pEvent->code)
        {
        case SYN_REPORT:
            /*
             * For identifiable MT, bDirty isn't set when handling MT events but
             *   only states are saved. First touching contact may lift first
             *   thus the first valid contact may not always 1st in pContactStat.
             *   So check and find the actual contacts to report, copy to final
             *   report buffer and set bDirty.
             * Anonymous MT already sets bDirty when seeing SYN_MT_REPORT.
             */
            if (pTabletDesc->bIdentifiableMT)
            {
                UCHAR uContacts = 0;

                pReport[HID_REPORT_DATA_OFFSET + sizeof(INPUT_CLASS_TABLET_SLOT) * pTabletDesc->uMaxContacts] = uContacts;
                for (uNumContacts = 0; uNumContacts < pTabletDesc->uMaxContacts; uNumContacts++)
                {
                    // 源数据可能不连续
                    if (pTabletDesc->pTrackingID[uNumContacts].uID != -1)
                    {
                        RtlCopyMemory(
                            &((PINPUT_CLASS_TABLET_SLOT)&pReport[HID_REPORT_DATA_OFFSET])[uContacts++],
                            &pTabletDesc->pContactStat[uNumContacts],
                            sizeof(INPUT_CLASS_TABLET_SLOT));
                    }
                }

                if (uContacts)
                {
                    // 记录个数
                    pReport[HID_REPORT_DATA_OFFSET + sizeof(INPUT_CLASS_TABLET_SLOT) * pTabletDesc->uMaxContacts] = uContacts;
                    // 设置dirty代表有新数据
                    pClass->bDirty = TRUE;

                    KIRQL irql;
                    KeAcquireSpinLock(&lastDataLock, &irql);

                    dataLen = HID_REPORT_DATA_OFFSET + sizeof(INPUT_CLASS_TABLET_SLOT) * pTabletDesc->uMaxContacts + 1;
                    RtlCopyMemory(data, pReport, dataLen);

                    KeReleaseSpinLock(&lastDataLock, irql);
                }

                TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "Report first byte : %d\n", pReport[0]);
                TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "Report contacts count : %d\n", uContacts);

                PINPUT_CLASS_TABLET_SLOT pTabltItem = (PINPUT_CLASS_TABLET_SLOT)&pReport[HID_REPORT_DATA_OFFSET];

                TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "uFlags:     %d\n", pTabltItem->uFlags);
                TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "uContactID: %d\n", pTabltItem->uContactID);
                TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "uAxisX:     %d\n", pTabltItem->uAxisX);
                TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "uAxisY:     %d\n", pTabltItem->uAxisY);
                TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "uWidth:     %d\n", pTabltItem->uWidth);
                TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "uHeight:    %d\n", pTabltItem->uHeight);
            }
            break;
        default:
            break;
        }
        break;
    default:
        break;
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "<-- %s\n", __FUNCTION__);

    return STATUS_SUCCESS;
}

static NTSTATUS
HIDTabletEventToReport(
    PINPUT_CLASS_COMMON pClass,
    PVIRTIO_INPUT_EVENT pEvent)
{
    PINPUT_CLASS_TABLET pTabletDesc = (PINPUT_CLASS_TABLET)pClass;
    PUCHAR pReport = pClass->pHidReport;
    PUSHORT pAxisReport;
    PINPUT_CLASS_TABLET_SLOT pReportSlot;
    UCHAR uBits;
    ULONG uNumContacts;

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "--> %s\n", __FUNCTION__);

    pReport[HID_REPORT_ID_OFFSET] = pClass->uReportID;
    switch (pEvent->type)
    {
    case EV_ABS:
        if (pTabletDesc->bMT)
        {
            /*
             * For identifiable MT, contact event are firstly saved into
             *   pContactStat then copied to report buffer by valid tracking ID.
             * For anonymous MT, contact event are directly saved into report
             *   buffer one by one on seeing SYN_MT_REPORT.
             */
            if (pTabletDesc->bIdentifiableMT)
            {
                pReportSlot = &pTabletDesc->pContactStat[pTabletDesc->uLastMTSlot];
            }
            else
            {
                pReportSlot = &((PINPUT_CLASS_TABLET_SLOT)&pReport[HID_REPORT_DATA_OFFSET])[pTabletDesc->uLastMTSlot];
            }
            switch (pEvent->code)
            {
            case ABS_MT_SLOT:
                /*
                 * Subsequent identifiable MT event will re-use last set MT_SLOT
                 *   until new slot arrives so we need save it for later usage
                 *   and keep using uLastMTSlot as current slot for other operation.
                 * Only identifiable MT will send MT_SLOT. Still add protection
                 *   in case back-end somehow goes wrong.
                 */
                if (pTabletDesc->bIdentifiableMT)
                {
                    if (pEvent->value < pTabletDesc->uMaxContacts)
                    {
                        pTabletDesc->uLastMTSlot = (ULONG)pEvent->value;
                    }
                    else
                    {
                        pTabletDesc->uLastMTSlot = 0;
                    }
                }
                break;
            case ABS_MT_TOUCH_MAJOR:
                pReportSlot->uWidth = (USHORT)pEvent->value;
                break;
            case ABS_MT_TOUCH_MINOR:
                pReportSlot->uHeight = (USHORT)pEvent->value;
                break;
            case ABS_MT_POSITION_X:
                pReportSlot->uAxisX = (USHORT)pEvent->value;
                break;
            case ABS_MT_POSITION_Y:
                pReportSlot->uAxisY = (USHORT)pEvent->value;
                break;
            case ABS_MT_TRACKING_ID:
                /*
                 * Check if negative tracking ID for actual contact up & down.
                 * Contact ID is bind to slot until changed, save it to operate
                 * subsequent MT event. In case of negative tracking ID, don't
                 * mark as unused slot with -1 but mark as pending only so that
                 * the contact can be reported lift up on EN_SYN and unset then.
                 */
                if ((LONG)pEvent->value < 0)
                {
                    pTabletDesc->pTrackingID[pTabletDesc->uLastMTSlot].bPendingDel = TRUE;
                    pReportSlot->uFlags &= ~0x01;
                }
                else
                {
                    pTabletDesc->pTrackingID[pTabletDesc->uLastMTSlot].uID = (LONG)pEvent->value;
                    pReportSlot->uContactID = (USHORT)pEvent->value;
                    pReportSlot->uFlags |= 0x01;
                }
                break;
            default:
                break;
            }
        }
        // ST
        else
        {
            /*
             * ST always fills ABS_X/ABS_Y and EV_KEY in 1st slot while
             * uLastMTSlot remains unchanged for all events.
             */
            pReportSlot = &((PINPUT_CLASS_TABLET_SLOT)&pReport[HID_REPORT_DATA_OFFSET])[pTabletDesc->uLastMTSlot];
            switch (pEvent->code)
            {
            case ABS_X:
                pReportSlot->uAxisX = (USHORT)pEvent->value;
                pClass->bDirty = TRUE;
                break;
            case ABS_Y:
                pReportSlot->uAxisY = (USHORT)pEvent->value;
                pClass->bDirty = TRUE;
                break;
            default:
                break;
            }
        }
        break;
    case EV_KEY:
        switch (pEvent->code)
        {
        case BTN_LEFT:
        case BTN_TOUCH:
            // tip switch + in range
            uBits = 0x03;
            break;
        case BTN_RIGHT:
        case BTN_STYLUS:
            // barrel switch
            uBits = 0x04;
            break;
        default:
            uBits = 0x00;
            break;
        }

        // MT will set bDirty before reporting at EV_SYN so drop all bits here.
        if (pTabletDesc->bMT)
        {
            uBits = 0x00;
        }

        if (uBits)
        {
            pReportSlot = &((PINPUT_CLASS_TABLET_SLOT)&pReport[HID_REPORT_DATA_OFFSET])[pTabletDesc->uLastMTSlot];
            if (pEvent->value)
            {
                pReportSlot->uFlags |= uBits;
            }
            else
            {
                pReportSlot->uFlags &= ~uBits;
            }
            pClass->bDirty = TRUE;
        }
        break;
    case EV_MSC:
        switch (pEvent->code)
        {
        case MSC_TIMESTAMP:
            if (pTabletDesc->bMscTs)
            {
                PLONG pScanTime = (PLONG)&pReport[HID_REPORT_DATA_OFFSET + sizeof(INPUT_CLASS_TABLET_SLOT) * pTabletDesc->uMaxContacts + 1];
                // Convert MSC_TIMESTAMP microseconds to 100 microseconds
                *pScanTime = ((ULONG)pEvent->value / 100);
            }
            break;
        default:
            break;
        }
        break;
    case EV_SYN:
        switch (pEvent->code)
        {
        case SYN_REPORT:
            /*
             * Post-processing SYN_REPORT after done reporting.
             * For ST, the num of contacts to report is always 1 so nothing to do.
             * For MT, clear the number of contacts to report so that up-to-date
             *   number can be re-count before reporting on next EV_SYN.
             *   For identifiable MT, clear pending tracking ID.
             *   For anonymous MT, clear uLastMTSlot which is increased on
             *     SYN_MT_REPORT.
             */
            if (pTabletDesc->bIdentifiableMT)
            {
                for (uNumContacts = 0; uNumContacts < pTabletDesc->uMaxContacts; uNumContacts++)
                {
                    if (pTabletDesc->pTrackingID[uNumContacts].bPendingDel)
                    {
                        pTabletDesc->pTrackingID[uNumContacts].uID = -1;
                        pTabletDesc->pTrackingID[uNumContacts].bPendingDel = FALSE;
                    }
                }
                pReport[HID_REPORT_DATA_OFFSET + sizeof(INPUT_CLASS_TABLET_SLOT) * pTabletDesc->uMaxContacts] = 0;
            }
            else if (pTabletDesc->bMT)
            {
                pTabletDesc->uLastMTSlot = 0;
                pReport[HID_REPORT_DATA_OFFSET + sizeof(INPUT_CLASS_TABLET_SLOT) * pTabletDesc->uMaxContacts] = 0;
            }
            break;
        case SYN_MT_REPORT:
            /*
             * Anonymous MT won't use MT_SLOT/MT_TRACKING_ID for each contacts,
             * so move to next slot when seeing SYN_MT_REPORT. If case of
             * overflow, round uLastMTSlot the 1st but keep number of contacts.
             */
            if (pTabletDesc->bMT && !pTabletDesc->bIdentifiableMT)
            {
                ++pTabletDesc->uLastMTSlot;
                if (pTabletDesc->uLastMTSlot <= pTabletDesc->uMaxContacts)
                {
                    ++pReport[HID_REPORT_DATA_OFFSET + sizeof(INPUT_CLASS_TABLET_SLOT) * pTabletDesc->uMaxContacts];
                }
                if (++pTabletDesc->uLastMTSlot >= pTabletDesc->uMaxContacts)
                {
                    pTabletDesc->uLastMTSlot = 0;
                }
                pClass->bDirty = TRUE;
            }
            break;
        default:
            break;
        }
        break;
    }

    TraceEvents(TRACE_LEVEL_VERBOSE, DBG_READ, "<-- %s\n", __FUNCTION__);

    return STATUS_SUCCESS;
}

static VOID
HIDTabletCleanup(
    PINPUT_CLASS_COMMON pClass)
{
    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "--> %s\n", __FUNCTION__);

    PINPUT_CLASS_TABLET pTabletDesc = (PINPUT_CLASS_TABLET)pClass;

    if (pTabletDesc->pContactStat)
    {
        VIOInputFree(&pTabletDesc->pContactStat);
    }
    if (pTabletDesc->pTrackingID)
    {
        VIOInputFree(&pTabletDesc->pTrackingID);
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "<-- %s\n", __FUNCTION__);
}

NTSTATUS
HIDTabletProbe(
    PINPUT_DEVICE pContext,
    PDYNAMIC_ARRAY pHidDesc,
    PVIRTIO_INPUT_CFG_DATA pAxes,
    PVIRTIO_INPUT_CFG_DATA pButtons,
    PVIRTIO_INPUT_CFG_DATA pMisc)
{
    PINPUT_CLASS_TABLET pTabletDesc = NULL;
    NTSTATUS status = STATUS_SUCCESS;
    UCHAR i, uValue;
    ULONG uAxisCode, uNumOfAbsAxes = 0, uNumOfMTAbsAxes = 0, uNumContacts = 0;

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "--> %s\n", __FUNCTION__);

    // allocate and initialize pTabletDesc
    pTabletDesc = VIOInputAlloc(sizeof(INPUT_CLASS_TABLET));
    if (pTabletDesc == NULL)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    // Init as single touch
    pTabletDesc->bMT = FALSE;
    pTabletDesc->bIdentifiableMT = FALSE;
    pTabletDesc->uMaxContacts = 1;

    // we expect to see two absolute axes, X and Y
    for (i = 0; i < pAxes->size; i++)
    {
        UCHAR uNonAxes = 0;
        while (DecodeNextBit(&pAxes->u.bitmap[i], &uValue))
        {
            USHORT uAxisCode = uValue + 8 * i;
            if (uAxisCode == ABS_X || uAxisCode == ABS_Y)
            {
                uNumOfAbsAxes++;
            }
            else if (uAxisCode == ABS_MT_SLOT)
            {
                struct virtio_input_absinfo AbsInfo;
                GetAbsAxisInfo(pContext, uAxisCode, &AbsInfo);

                pTabletDesc->uMaxContacts = AbsInfo.max + 1;
                if (pTabletDesc->uMaxContacts > MT_MAX_MAXCONTACT)
                {
                    pTabletDesc->uMaxContacts = MT_MAX_MAXCONTACT;
                    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "Back end report more contacts (%d) than front end can support (%d). Limit to (%d)\n",
                                AbsInfo.max + 1, MT_MAX_MAXCONTACT, MT_MAX_MAXCONTACT);
                }
                pTabletDesc->bMT = TRUE;
                pTabletDesc->bIdentifiableMT = TRUE;
            }
            else if ((uAxisCode >= ABS_MT_TOUCH_MAJOR) && (uAxisCode <= ABS_MT_TOOL_Y))
            {
                pTabletDesc->bMT = TRUE;
                if (uAxisCode == ABS_MT_POSITION_X || uAxisCode == ABS_MT_POSITION_Y)
                {
                    uNumOfMTAbsAxes++;
                }
                if (uAxisCode == ABS_MT_TRACKING_ID)
                {
                    pTabletDesc->bIdentifiableMT = TRUE;
                }
            }
            else
            {
                uNonAxes |= (1 << uValue);
            }
        }
        pAxes->u.bitmap[i] = uNonAxes;
    }

    if (uNumOfAbsAxes != 2)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "Tablet axes not found\n");
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    pTabletDesc->bMscTs = FALSE;
    for (i = 0; i < pMisc->size; i++)
    {
        while (DecodeNextBit(&pMisc->u.bitmap[i], &uValue))
        {
            UCHAR msc_event = uValue + 8 * i;
            if (msc_event == MSC_TIMESTAMP)
            {
                pTabletDesc->bMscTs = TRUE;
            }
        }
    }

    /*
     * MT could be type A (anonymous contacts) or type B (identifiable contacts)
     * For anonymous MT, seeing another SYN_MT_REPORT indicates a new contact
     *   in same report.
     * For identifiable MT, ABS_MT_SLOT and ABS_MT_TRACKING_ID are used to
     *   identify the contact number and identity.
     * If we got type A, uMaxContacts can't be parsed from ABS_MT_SLOT thus
     *   limit to MT_DEFAULT_MAXCONTACT.
     */
    if (!pTabletDesc->bIdentifiableMT && pTabletDesc->bMT)
    {
        pTabletDesc->uMaxContacts = MT_DEFAULT_MAXCONTACT;
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "Type A (identifiable contacts) MT maximum contacts is limited to (%d). Consider to increase MT_DEFAULT_MAXCONTACT if necessary.\n",
                    MT_DEFAULT_MAXCONTACT);
    }

    if (pTabletDesc->bMT && uNumOfMTAbsAxes != 2)
    {
        pTabletDesc->bMT = FALSE;
        pTabletDesc->bIdentifiableMT = FALSE;
        pTabletDesc->uMaxContacts = 1;
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "Got MT abs info but doesn't have ABS_MT_POSITION_X and ABS_MT_POSITION_Y, fall back to ST\n");
    }

    if (pTabletDesc->uMaxContacts > MT_MAX_MAXCONTACT)
    {
        TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "Backend report more maximum contacts (%d) than frontend can support (%d), limit to (%d). Consider to increase MT_MAX_MAXCONTACT if necessary.\n",
                    pTabletDesc->uMaxContacts, MT_MAX_MAXCONTACT, MT_MAX_MAXCONTACT);
        pTabletDesc->uMaxContacts = MT_MAX_MAXCONTACT;
    }

    // Simulate as ST for test
    //pTabletDesc->bMT = FALSE;
    //pTabletDesc->bIdentifiableMT = FALSE;
    //pTabletDesc->uMaxContacts = 1;

    // Allocate and all contact status for MT
    if (pTabletDesc->bMT)
    {
        pTabletDesc->pContactStat = VIOInputAlloc(sizeof(INPUT_CLASS_TABLET_SLOT) * pTabletDesc->uMaxContacts);
        if (pTabletDesc->pContactStat == NULL)
        {
            status = STATUS_INSUFFICIENT_RESOURCES;
            goto Exit;
        }
    }

    pTabletDesc->pTrackingID = VIOInputAlloc(sizeof(INPUT_CLASS_TABLET_TRACKING_ID) * pTabletDesc->uMaxContacts);
    if (pTabletDesc->pTrackingID == NULL)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }

    pTabletDesc->uLastMTSlot = 0;
    for (uNumContacts = 0; uNumContacts < pTabletDesc->uMaxContacts; uNumContacts++)
    {
        pTabletDesc->pTrackingID[uNumContacts].uID = -1;
        pTabletDesc->pTrackingID[uNumContacts].bPendingDel = FALSE;
        if (pTabletDesc->bMT)
        {
            pTabletDesc->pContactStat[uNumContacts].uWidth = 1;
            pTabletDesc->pContactStat[uNumContacts].uHeight = 1;
        }
    }

    // claim our buttons from the pAxes bitmap
    for (i = 0; i < pButtons->size; i++)
    {
        UCHAR uNonButtons = 0;
        while (DecodeNextBit(&pAxes->u.bitmap[i], &uValue))
        {
            USHORT uAxisCode = uValue + 8 * i;
            // a few hard-coded buttons we understand
            switch (uAxisCode)
            {
            case BTN_LEFT:
            case BTN_RIGHT:
            case BTN_TOUCH:
            case BTN_STYLUS:
                break;
            default:
                uNonButtons |= (1 << uValue);
            }
        }
        pAxes->u.bitmap[i] = uNonButtons;
    }

    pTabletDesc->Common.GetFeatureFunc = HIDTabletGetFeature;
    pTabletDesc->Common.EventToCollectFunc = HIDTabletEventToCollect;
    pTabletDesc->Common.EventToReportFunc = HIDTabletEventToReport;
    pTabletDesc->Common.CleanupFunc = HIDTabletCleanup;
    pTabletDesc->Common.uReportID = (UCHAR)(pContext->uNumOfClasses + 1);

    HIDAppend2(pHidDesc, HID_TAG_USAGE_PAGE, HID_USAGE_PAGE_DIGITIZER);
    HIDAppend2(pHidDesc, HID_TAG_USAGE, pTabletDesc->bMT ? HID_USAGE_TOUCH_SCREEN : HID_USAGE_DIGITIZER);

    HIDAppend2(pHidDesc, HID_TAG_COLLECTION, HID_COLLECTION_APPLICATION);
    HIDAppend2(pHidDesc, HID_TAG_REPORT_ID, pTabletDesc->Common.uReportID);
    HIDAppend2(pHidDesc, HID_TAG_USAGE, pTabletDesc->bMT ? HID_USAGE_DIGITIZER_FINGER : HID_USAGE_DIGITIZER_STYLUS);

    for (uNumContacts = 0; uNumContacts < pTabletDesc->uMaxContacts; uNumContacts++)
    {
        // Collection Logical for all contacts for reporting in hybrid mode
        HIDAppend2(pHidDesc, HID_TAG_COLLECTION, HID_COLLECTION_LOGICAL);

        // Change to digitizer page for touch
        HIDAppend2(pHidDesc, HID_TAG_USAGE_PAGE, HID_USAGE_PAGE_DIGITIZER);

        // Same logical minimum and maximum applied to below flags, 1 bit each, 1 byte total
        HIDAppend2(pHidDesc, HID_TAG_LOGICAL_MINIMUM, 0x00);
        HIDAppend2(pHidDesc, HID_TAG_LOGICAL_MAXIMUM, 0x01);
        HIDAppend2(pHidDesc, HID_TAG_REPORT_SIZE, 0x01);
        HIDAppend2(pHidDesc, HID_TAG_REPORT_COUNT, 0x01);

        // tip switch, one bit
        HIDAppend2(pHidDesc, HID_TAG_USAGE, HID_USAGE_DIGITIZER_TIP_SWITCH);
        HIDAppend2(pHidDesc, HID_TAG_INPUT, HID_DATA_FLAG_VARIABLE);

        if (!pTabletDesc->bMT)
        {
            // in range flag, one bit
            HIDAppend2(pHidDesc, HID_TAG_USAGE, HID_USAGE_DIGITIZER_IN_RANGE);
            HIDAppend2(pHidDesc, HID_TAG_INPUT, HID_DATA_FLAG_VARIABLE);

            // barrel switch, one bit
            HIDAppend2(pHidDesc, HID_TAG_USAGE, HID_USAGE_DIGITIZER_BARREL_SWITCH);
            HIDAppend2(pHidDesc, HID_TAG_INPUT, HID_DATA_FLAG_VARIABLE);
        }

        // padding
        HIDAppend2(pHidDesc, HID_TAG_LOGICAL_MAXIMUM, 0x00);
        HIDAppend2(pHidDesc, HID_TAG_REPORT_COUNT, pTabletDesc->bMT ? 0x07 : 0x05);
        HIDAppend2(pHidDesc, HID_TAG_INPUT, HID_DATA_FLAG_VARIABLE | HID_DATA_FLAG_CONSTANT);

        // Contact Identifier, 2 bytes
        HIDAppend2(pHidDesc, HID_TAG_REPORT_SIZE, 0x10);
        HIDAppend2(pHidDesc, HID_TAG_REPORT_COUNT, 0x01);
        HIDAppend2(pHidDesc, HID_TAG_USAGE, HID_USAGE_DIGITIZER_CONTACT_ID);
        HIDAppend2(pHidDesc, HID_TAG_LOGICAL_MAXIMUM, pTabletDesc->uMaxContacts - 1);
        HIDAppend2(pHidDesc, HID_TAG_INPUT, HID_DATA_FLAG_VARIABLE);

        // Change to generic desktop page for coordinates
        HIDAppend2(pHidDesc, HID_TAG_USAGE_PAGE, HID_USAGE_PAGE_GENERIC);
        HIDAppend2(pHidDesc, HID_TAG_UNIT_EXPONENT, 0x0E); // 10^(-2)
        HIDAppend2(pHidDesc, HID_TAG_UNIT, 0x11);          // Linear Centimeter

        // 2 bytes each axis
        for (uAxisCode = ABS_X; uAxisCode <= ABS_Y; uAxisCode++)
        {
            struct virtio_input_absinfo AbsInfo;
            GetAbsAxisInfo(pContext, uAxisCode, &AbsInfo);

            TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "Got abs axis %d, min %d, max %d\n",
                        uAxisCode, AbsInfo.min, AbsInfo.max);

            // logical == physical, no offset and scale
            // Device Class Definition for HID 1.11, 6.2.2.7
            // Resolution = (Logical Maximum - Logical Minimum) /
            //    ((Physical Maximum - Physical Minimum) * (10 Unit Exponent))
            // struct input_absinfo{}.res reports in units/mm, convert to cm.
            HIDAppend2(pHidDesc, HID_TAG_LOGICAL_MINIMUM, AbsInfo.min);
            HIDAppend2(pHidDesc, HID_TAG_LOGICAL_MAXIMUM, AbsInfo.max);
            HIDAppend2(pHidDesc, HID_TAG_PHYSICAL_MINIMUM,
                (AbsInfo.res == 0) ? AbsInfo.min : (AbsInfo.min * 10 / AbsInfo.res));
            HIDAppend2(pHidDesc, HID_TAG_PHYSICAL_MAXIMUM,
                (AbsInfo.res == 0) ? AbsInfo.max : (AbsInfo.max * 10 / AbsInfo.res));

            HIDAppend2(pHidDesc, HID_TAG_USAGE,
                (uAxisCode == ABS_X ? HID_USAGE_GENERIC_X : HID_USAGE_GENERIC_Y));

            HIDAppend2(pHidDesc, HID_TAG_INPUT, HID_DATA_FLAG_VARIABLE);
        }

        // 2 bytes for contact width/height
        HIDAppend2(pHidDesc, HID_TAG_USAGE_PAGE, HID_USAGE_PAGE_DIGITIZER);
        HIDAppend2(pHidDesc, HID_TAG_USAGE, HID_USAGE_DIGITIZER_CONTACT_WIDTH);
        HIDAppend2(pHidDesc, HID_TAG_INPUT, HID_DATA_FLAG_VARIABLE);
        HIDAppend2(pHidDesc, HID_TAG_USAGE, HID_USAGE_DIGITIZER_CONTACT_HEIGHT);
        HIDAppend2(pHidDesc, HID_TAG_INPUT, HID_DATA_FLAG_VARIABLE);

        HIDAppend1(pHidDesc, HID_TAG_END_COLLECTION); //HID_COLLECTION_LOGICAL
    }

    // Change to digitizer page for contacts information
    HIDAppend2(pHidDesc, HID_TAG_USAGE_PAGE, HID_USAGE_PAGE_DIGITIZER);

    // Same range for both number of contacts in current report and maximum number of supported contacts, 1 byte each
    HIDAppend2(pHidDesc, HID_TAG_LOGICAL_MINIMUM, 0x00);
    HIDAppend2(pHidDesc, HID_TAG_LOGICAL_MAXIMUM, pTabletDesc->uMaxContacts);
    HIDAppend2(pHidDesc, HID_TAG_REPORT_SIZE, 0x08);
    HIDAppend2(pHidDesc, HID_TAG_REPORT_COUNT, 0x01);

    HIDAppend2(pHidDesc, HID_TAG_USAGE, HID_USAGE_DIGITIZER_CONTACT_COUNT);
    HIDAppend2(pHidDesc, HID_TAG_INPUT, HID_DATA_FLAG_VARIABLE);

    if (pTabletDesc->bMscTs)
    {
        HIDAppend2(pHidDesc, HID_TAG_USAGE, HID_USAGE_DIGITIZER_SCAN_TIME);
        HIDAppend2(pHidDesc, HID_TAG_UNIT_EXPONENT, 0x0A); // 10^(-4), 100 us
        HIDAppend2(pHidDesc, HID_TAG_UNIT, 0x1001); // Time system in unit of s
        HIDAppend2(pHidDesc, HID_TAG_LOGICAL_MAXIMUM, LONG_MAX);
        HIDAppend2(pHidDesc, HID_TAG_REPORT_SIZE, 0x20);
        HIDAppend2(pHidDesc, HID_TAG_REPORT_COUNT, 0x01);
        HIDAppend2(pHidDesc, HID_TAG_INPUT, HID_DATA_FLAG_VARIABLE);

        // Restore for subsequent item
        HIDAppend2(pHidDesc, HID_TAG_UNIT_EXPONENT, 0x00);
        HIDAppend2(pHidDesc, HID_TAG_UNIT, 0x00);
        HIDAppend2(pHidDesc, HID_TAG_LOGICAL_MAXIMUM, pTabletDesc->uMaxContacts);
        HIDAppend2(pHidDesc, HID_TAG_REPORT_SIZE, 0x08);
        HIDAppend2(pHidDesc, HID_TAG_REPORT_COUNT, 0x01);
    }

    // IOCTL_HID_GET_FEATURE will query the report for maximum count
    HIDAppend2(pHidDesc, HID_TAG_REPORT_ID, REPORTID_FEATURE_TABLET_MAX_COUNT);
    HIDAppend2(pHidDesc, HID_TAG_USAGE, HID_USAGE_DIGITIZER_CONTACT_COUNT_MAX);
    HIDAppend2(pHidDesc, HID_TAG_FEATURE, HID_DATA_FLAG_VARIABLE | HID_DATA_FLAG_CONSTANT);

    HIDAppend1(pHidDesc, HID_TAG_END_COLLECTION); //HID_COLLECTION_APPLICATION

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "Created HID tablet report descriptor\n");

    // calculate the tablet HID report size
    pTabletDesc->Common.cbHidReportSize =
        1 + // report ID
        sizeof(INPUT_CLASS_TABLET_SLOT) * pTabletDesc->uMaxContacts + // max contacts * per-contact packet. See INPUT_CLASS_TABLET_SLOT and INPUT_CLASS_TABLET for layout details.
        1 + // Actual contact count
        (pTabletDesc->bMscTs ? sizeof(LONG) : 0) // Scan time
        ;

    // register the tablet class
    status = RegisterClass(pContext, &pTabletDesc->Common);
    if (NT_SUCCESS(status))
    {
        PUCHAR pReport = pTabletDesc->Common.pHidReport;

        /*
         * For ST, the number of contacts to report is always 1.
         * For MT, the number of contacts to report is counted at SYN_REPORT.
         */
        if (pTabletDesc->bMT)
        {
            pReport[HID_REPORT_DATA_OFFSET + sizeof(INPUT_CLASS_TABLET_SLOT) * pTabletDesc->uMaxContacts] = 0;
        }
        else
        {
            pReport[HID_REPORT_DATA_OFFSET + sizeof(INPUT_CLASS_TABLET_SLOT) * pTabletDesc->uMaxContacts] = 1;
            ((PINPUT_CLASS_TABLET_SLOT)&pReport[HID_REPORT_DATA_OFFSET])->uWidth = 1;
            ((PINPUT_CLASS_TABLET_SLOT)&pReport[HID_REPORT_DATA_OFFSET])->uHeight = 1;
        }
    }

Exit:
    if (!NT_SUCCESS(status) && pTabletDesc)
    {
        if (pTabletDesc->pContactStat)
        {
            VIOInputFree(&pTabletDesc->pContactStat);
        }
        if (pTabletDesc->pTrackingID)
        {
            VIOInputFree(&pTabletDesc->pTrackingID);
        }
        VIOInputFree(&pTabletDesc);
    }

    TraceEvents(TRACE_LEVEL_INFORMATION, DBG_INIT, "<-- %s (%08x)\n", __FUNCTION__, status);
    return status;
}
