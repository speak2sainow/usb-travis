/* libusb-win32 WDF, Generic KMDF Windows USB Driver
 * Copyright (c) 2010-2011 Travis Robinson <libusbdotnet@gmail.com>
 * Copyright (c) 2002-2005 Stephan Meyer <ste_meyer@web.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "private.h"

VOID
DoSubRequestCleanup(
    PREQUEST_CONTEXT    MainRequestContext,
    PLIST_ENTRY         SubRequestsList,
    PBOOLEAN            CompleteRequest
)  ;

VOID TransferIsoFS(
    IN WDFQUEUE         Queue,
    IN WDFREQUEST       Request,
    IN ULONG            Length,
    IN WDF_REQUEST_TYPE RequestType,
    IN PMDL				TransferMDL,
    IN PPIPE_CONTEXT	PipeContext
)
/*++

Routine Description:

    This routine
    1. Creates a Sub Request for each irp/urb pair.
       (Each irp/urb pair can transfer a max of 1024 packets.)
    2. All the irp/urb pairs are initialized
    3. The subsidiary irps (of the irp/urb pair) are passed
       down the stack at once.
    4. The main Read/Write is completed in the SubRequestCompletionRoutine
       even if one SubRequest is sent successfully.

Arguments:

    Device - Device handle
    Request - Default queue handle
    Request - Read/Write Request received from the user app.
    TotalLength - Length of the user buffer.

Return Value:

    VOID
--*/
{
	ULONG                   packetSize;
	ULONG                   numSubRequests;
	ULONG                   stageSize;
	PUCHAR                  virtualAddress;
	NTSTATUS               status;
	PDEVICE_CONTEXT         deviceContext;
	PREQUEST_CONTEXT        rwContext;
	WDFCOLLECTION           hCollection = NULL;
	WDFSPINLOCK            hSpinLock = NULL;
	WDF_OBJECT_ATTRIBUTES   attributes;
	WDFREQUEST              subRequest = NULL;
	PSUB_REQUEST_CONTEXT    subReqContext = NULL;
	ULONG                   i, j;
	PLIST_ENTRY             thisEntry;
	LIST_ENTRY              subRequestsList;
	USBD_PIPE_HANDLE        usbdPipeHandle;
	BOOLEAN                 cancelable;
	ULONG					TotalLength;

	cancelable = FALSE;
	TotalLength = Length;

	InitializeListHead(&subRequestsList);

	rwContext = GetRequestContext(Request);


	if(RequestType == WdfRequestTypeRead)
	{

		rwContext->Read = TRUE;
	}
	else
	{
		rwContext->Read = FALSE;
	}

	USBMSG("%s - begins\n",
	       rwContext->Read ? "Read":"Write");

	deviceContext = GetDeviceContext(WdfIoQueueGetDevice(Queue));

	//
	// Create a collection to store all the sub requests.
	//
	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = Request;
	status = WdfCollectionCreate(&attributes,
	                             &hCollection);
	if (!NT_SUCCESS(status))
	{
		USBERR("WdfCollectionCreate failed. status=%Xh\n", status);
		goto Exit;
	}

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = hCollection;
	status = WdfSpinLockCreate(&attributes, &hSpinLock);
	if (!NT_SUCCESS(status))
	{
		USBERR("WdfSpinLockCreate failed. status=%Xh\n", status);
		goto Exit;
	}


	//
	// Each packet can hold this much info
	//
	packetSize = PipeContext->PipeInformation.MaximumPacketSize;

	USBMSG("totalLength = %d\n", TotalLength);
	USBMSG("packetSize = %d\n", packetSize);

	//
	// there is an inherent limit on the number of packets
	// that can be passed down the stack with each
	// irp/urb pair (255)
	// if the number of required packets is > 255,
	// we shall create "required-packets / 255 + 1" number
	// of irp/urb pairs.
	// Each irp/urb pair transfer is also called a stage transfer.
	//
	if(TotalLength > (packetSize * 255))
	{

		stageSize = packetSize * 255;
	}
	else
	{

		stageSize = TotalLength;
	}

	USBMSG("stageSize = %d\n", stageSize);

	//
	// determine how many stages of transfer needs to be done.
	// in other words, how many irp/urb pairs required.
	// this irp/urb pair is also called the subsidiary irp/urb pair
	//
	numSubRequests = (TotalLength + stageSize - 1) / stageSize;

	USBMSG("numSubRequests = %d\n", numSubRequests);

	rwContext->SubRequestCollection = hCollection;
	rwContext->SubRequestCollectionLock = hSpinLock;
	rwContext->OriginalTransferMDL = TransferMDL;

	virtualAddress = (PUCHAR) MmGetMdlVirtualAddress(TransferMDL);

	for(i = 0; i < numSubRequests; i++)
	{

		WDFMEMORY               subUrbMemory;
		PURB                    subUrb;
		PMDL                    subMdl;
		ULONG                   nPackets;
		ULONG                   siz;
		ULONG                   offset;

		//
		// For every stage of transfer we need to do the following
		// tasks
		// 1. allocate a request
		// 2. allocate an urb
		// 3. allocate a mdl.
		// 4. Format the request for transfering URB
		// 5. Send the Request.

		//

		nPackets = (stageSize + packetSize - 1) / packetSize;

		USBMSG("nPackets = %d for Irp/URB pair %d\n", nPackets, i);

		ASSERT(nPackets <= 255);
		siz = GET_ISO_URB_SIZE(nPackets);

		WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
		WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(&attributes, SUB_REQUEST_CONTEXT);

		status = WdfRequestCreate(
		             &attributes,
		             WdfUsbTargetDeviceGetIoTarget(deviceContext->WdfUsbTargetDevice),
		             &subRequest);

		if(!NT_SUCCESS(status))
		{

			USBERR("WdfRequestCreate failed. status=%Xh\n", status);
			goto Exit;
		}

		subReqContext = GetSubRequestContext(subRequest);
		subReqContext->UserRequest = Request;

		//
		// Allocate memory for URB.
		//
		WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
		attributes.ParentObject = subRequest;
		status = WdfMemoryCreate(
		             &attributes,
		             NonPagedPool,
		             POOL_TAG,
		             siz,
		             &subUrbMemory,
		             (PVOID*) &subUrb);

		if (!NT_SUCCESS(status))
		{
			WdfObjectDelete(subRequest);
			USBERR0("Failed to alloc MemoryBuffer for suburb\n");
			goto Exit;
		}

		subReqContext->SubUrb = subUrb;

		//
		// Allocate a mdl and build the MDL to describe the staged buffer.
		//
		subMdl = IoAllocateMdl((PVOID) virtualAddress,
		                       stageSize,
		                       FALSE,
		                       FALSE,
		                       NULL);

		if(subMdl == NULL)
		{

			USBERR0("failed to alloc mem for sub context mdl\n");
			WdfObjectDelete(subRequest);
			status = STATUS_INSUFFICIENT_RESOURCES;
			goto Exit;
		}

		IoBuildPartialMdl(TransferMDL,
		                  subMdl,
		                  (PVOID) virtualAddress,
		                  stageSize);

		subReqContext->SubMdl = subMdl;

		virtualAddress += stageSize;
		TotalLength -= stageSize;

		//
		// Initialize the subsidiary urb
		//
		usbdPipeHandle = WdfUsbTargetPipeWdmGetPipeHandle(PipeContext->Pipe);

		subUrb->UrbIsochronousTransfer.Hdr.Length = (USHORT) siz;
		subUrb->UrbIsochronousTransfer.Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
		subUrb->UrbIsochronousTransfer.PipeHandle = usbdPipeHandle;
		if(rwContext->Read)
		{

			subUrb->UrbIsochronousTransfer.TransferFlags =
			    USBD_TRANSFER_DIRECTION_IN;
		}
		else
		{

			subUrb->UrbIsochronousTransfer.TransferFlags =
			    USBD_TRANSFER_DIRECTION_OUT;
		}

		subUrb->UrbIsochronousTransfer.TransferBufferLength = stageSize;
		subUrb->UrbIsochronousTransfer.TransferBufferMDL = subMdl;

#if 0
		//
		// This is a way to set the start frame and NOT specify ASAP flag.
		//
		status = WdfUsbTargetDeviceRetrieveCurrentFrameNumber(wdfUsbDevice, &frameNumber);
		subUrb->UrbIsochronousTransfer.StartFrame = frameNumber  + SOME_LATENCY;
#endif

		//
		// when the client driver sets the ASAP flag, it basically
		// guarantees that it will make data available to the HC
		// and that the HC should transfer it in the next transfer frame
		// for the endpoint.(The HC maintains a next transfer frame
		// state variable for each endpoint). If the data does not get to the HC
		// fast enough, the USBD_ISO_PACKET_DESCRIPTOR - Status is
		// USBD_STATUS_BAD_START_FRAME on uhci. On ohci it is 0xC000000E.
		//

		subUrb->UrbIsochronousTransfer.TransferFlags |=
		    USBD_START_ISO_TRANSFER_ASAP;

		subUrb->UrbIsochronousTransfer.NumberOfPackets = nPackets;
		subUrb->UrbIsochronousTransfer.UrbLink = NULL;

		//
		// set the offsets for every packet for reads/writes
		//
		if(rwContext->Read)
		{

			offset = 0;

			for(j = 0; j < nPackets; j++)
			{

				subUrb->UrbIsochronousTransfer.IsoPacket[j].Offset = offset;
				subUrb->UrbIsochronousTransfer.IsoPacket[j].Length = 0;

				if(stageSize > packetSize)
				{

					offset += packetSize;
					stageSize -= packetSize;
				}
				else
				{

					offset += stageSize;
					stageSize = 0;
				}
			}
		}
		else
		{

			offset = 0;

			for(j = 0; j < nPackets; j++)
			{

				subUrb->UrbIsochronousTransfer.IsoPacket[j].Offset = offset;

				if(stageSize > packetSize)
				{

					subUrb->UrbIsochronousTransfer.IsoPacket[j].Length = packetSize;
					offset += packetSize;
					stageSize -= packetSize;
				}
				else
				{

					subUrb->UrbIsochronousTransfer.IsoPacket[j].Length = stageSize;
					offset += stageSize;
					stageSize = 0;
					ASSERT(offset == (subUrb->UrbIsochronousTransfer.IsoPacket[j].Length +
					                  subUrb->UrbIsochronousTransfer.IsoPacket[j].Offset));
				}
			}
		}


		//
		// Associate the URB with the request.
		//
		status = WdfUsbTargetPipeFormatRequestForUrb(PipeContext->Pipe,
		         subRequest,
		         subUrbMemory,
		         NULL);

		if (!NT_SUCCESS(status))
		{
			USBERR0("Failed to format requset for urb\n");
			WdfObjectDelete(subRequest);
			IoFreeMdl(subMdl);
			goto Exit;
		}

		WdfRequestSetCompletionRoutine(subRequest,
		                               SubRequestCompletionRoutine,
		                               rwContext);

		if(TotalLength > (packetSize * 255))
		{

			stageSize = packetSize * 255;
		}
		else
		{

			stageSize = TotalLength;
		}

		//
		// WdfCollectionAdd takes a reference on the request object and removes
		// it when you call WdfCollectionRemove.
		//
		status = WdfCollectionAdd(hCollection, subRequest);
		if (!NT_SUCCESS(status))
		{
			USBERR("WdfCollectionAdd failed. status=%Xh\n", status);
			WdfObjectDelete(subRequest);
			IoFreeMdl(subMdl);
			goto Exit;
		}

		InsertTailList(&subRequestsList, &subReqContext->ListEntry);

	}

	//
	// There is a subtle race condition which can happen if the cancel
	// routine is running while the sub-request completion routine is
	// trying to mark the request as uncancellable followed by completing
	// the request.
	//
	// We take a reference to prevent the above race condition.
	// The reference is released in the sub-request completion routine
	// if the request wasn't cancelled or in the cancel routine if it was.
	//
	WdfObjectReference(Request);

	//
	// Mark the main request cancelable so that we can cancel the subrequests
	// if the main requests gets cancelled for any reason.
	//
	WdfRequestMarkCancelable(Request, LUsbW_EvtRequestCancel);
	cancelable = TRUE;

	while(!IsListEmpty(&subRequestsList))
	{

		thisEntry = RemoveHeadList(&subRequestsList);
		subReqContext = CONTAINING_RECORD(thisEntry, SUB_REQUEST_CONTEXT, ListEntry);
		subRequest = WdfObjectContextGetObject(subReqContext);
		USBMSG("Sending subRequest 0x%p\n", subRequest);
		if (WdfRequestSend(subRequest, WdfUsbTargetPipeGetIoTarget(PipeContext->Pipe), WDF_NO_SEND_OPTIONS) == FALSE)
		{
			status = WdfRequestGetStatus(subRequest);
			//
			// Insert the subrequest back into the subrequestlist so cleanup can find it and delete it
			//
			InsertHeadList(&subRequestsList, &subReqContext->ListEntry);
			USBMSG("WdfRequestSend failed. status=%Xh\n", status);
			ASSERT(!NT_SUCCESS(status));
			goto Exit;
		}

		//
		// Don't touch the subrequest after it has been sent.
		// Make a note that at least one subrequest is sent. This will be used
		// in deciding whether we should free the subrequests in case of failure.
		//


	}
Exit:

	//
	// Checking the status besides the number of list entries will help differentiate
	// failures where everything succeeded vs where there were failures before adding
	// list entries.
	//
	if(NT_SUCCESS(status) && IsListEmpty(&subRequestsList))
	{
		//
		// We will let the completion routine to cleanup and complete the
		// main request.
		//
		return;
	}
	else
	{
		BOOLEAN  completeRequest;
		NTSTATUS tempStatus;

		completeRequest = TRUE;
		tempStatus = STATUS_SUCCESS;

		if(hCollection)
		{
			DoSubRequestCleanup(rwContext, &subRequestsList, &completeRequest);
		}

		if (completeRequest)
		{
			if (cancelable)
			{
				//
				// Mark the main request as not cancelable before completing it.
				//
				tempStatus = WdfRequestUnmarkCancelable(Request);
				if (NT_SUCCESS(tempStatus))
				{
					//
					// If WdfRequestUnmarkCancelable returns STATUS_SUCCESS
					// that means the cancel routine has been removed. In that case
					// we release the reference otherwise the cancel routine does it.
					//
					WdfObjectDereference(Request);
				}
			}

			if (rwContext->TotalTransferred > 0 )
			{
				WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, rwContext->TotalTransferred);
			}
			else
			{
				WdfRequestCompleteWithInformation(Request, status, rwContext->TotalTransferred);
			}
		}

	}

	USBMSG0("ends\n");
	return;
}

VOID TransferIsoHS(
    IN WDFQUEUE         Queue,
    IN WDFREQUEST       Request,
    IN ULONG            Length,
    IN WDF_REQUEST_TYPE RequestType,
    IN PMDL				TransferMDL,
    IN PPIPE_CONTEXT	PipeContext
)
/*++

Routine Description:

    High Speed Isoch Transfer requires packets in multiples of 8.
    (Argument: 8 micro-frames per ms frame)
    Another restriction is that each Irp/Urb pair can be associated
    with a max of 1024 packets.

    Here is one of the ways of creating Irp/Urb pairs.
    Depending on the characteristics of real-world device,
    the algorithm may be different

    This algorithm will distribute data evenly among all the packets.

    Input:
    TotalLength - no. of bytes to be transferred.

    Other parameters:
    packetSize - max size of each packet for this pipe.

    Implementation Details:

    Step 1:
    ASSERT(TotalLength >= 8)

    Step 2:
    Find the exact number of packets required to transfer all of this data

    numberOfPackets = (TotalLength + packetSize - 1) / packetSize

    Step 3:
    Number of packets in multiples of 8.

    if(0 == (numberOfPackets % 8)) {

        actualPackets = numberOfPackets;
    }
    else {

        actualPackets = numberOfPackets +
                        (8 - (numberOfPackets % 8));
    }

    Step 4:
    Determine the min. data in each packet.

    minDataInEachPacket = TotalLength / actualPackets;

    Step 5:
    After placing min data in each packet,
    determine how much data is left to be distributed.

    dataLeftToBeDistributed = TotalLength -
                              (minDataInEachPacket * actualPackets);

    Step 6:
    Start placing the left over data in the packets
    (above the min data already placed)

    numberOfPacketsFilledToBrim = dataLeftToBeDistributed /
                                  (packetSize - minDataInEachPacket);

    Step 7:
    determine if there is any more data left.

    dataLeftToBeDistributed -= (numberOfPacketsFilledToBrim *
                                (packetSize - minDataInEachPacket));

    Step 8:
    The "dataLeftToBeDistributed" is placed in the packet at index
    "numberOfPacketsFilledToBrim"

    Algorithm at play:

    TotalLength  = 8193
    packetSize   = 8
    Step 1

    Step 2
    numberOfPackets = (8193 + 8 - 1) / 8 = 1025

    Step 3
    actualPackets = 1025 + 7 = 1032

    Step 4
    minDataInEachPacket = 8193 / 1032 = 7 bytes

    Step 5
    dataLeftToBeDistributed = 8193 - (7 * 1032) = 969.

    Step 6
    numberOfPacketsFilledToBrim = 969 / (8 - 7) = 969.

    Step 7
    dataLeftToBeDistributed = 969 - (969 * 1) = 0.

    Step 8
    Done :)

    Another algorithm
    Completely fill up (as far as possible) the early packets.
    Place 1 byte each in the rest of them.
    Ensure that the total number of packets is multiple of 8.

    This routine then
    1. Creates a Sub Request for each irp/urb pair.
       (Each irp/urb pair can transfer a max of 1024 packets.)
    2. All the irp/urb pairs are initialized
    3. The subsidiary irps (of the irp/urb pair) are passed
       down the stack at once.
    4. The main Read/Write is completed in the SubRequestCompletionRoutine
       even if one SubRequest is sent successfully.

Arguments:

    Device - Device handle
    Request - Default queue handle
    Request - Read/Write Request received from the user app.
    TotalLength - Length of the user buffer.

Return Value:

    VOID
--*/
{
	ULONG                   numberOfPackets;
	ULONG                   actualPackets;
	ULONG                   minDataInEachPacket;
	ULONG                   dataLeftToBeDistributed;
	ULONG                   numberOfPacketsFilledToBrim;
	ULONG                   packetSize;
	ULONG                   numSubRequests;
	ULONG                   stageSize;
	PUCHAR                  virtualAddress;
	NTSTATUS                status;
	PDEVICE_CONTEXT         deviceContext;
	PREQUEST_CONTEXT        rwContext;
	WDFCOLLECTION           hCollection = NULL;
	WDFSPINLOCK             hSpinLock = NULL;
	WDF_OBJECT_ATTRIBUTES   attributes;
	WDFREQUEST              subRequest;
	PSUB_REQUEST_CONTEXT    subReqContext;
	ULONG                   i, j;
	PLIST_ENTRY             thisEntry;
	LIST_ENTRY              subRequestsList;
	USBD_PIPE_HANDLE        usbdPipeHandle;
	BOOLEAN                 cancelable;
	ULONG					TotalLength;

	cancelable = FALSE;
	TotalLength = Length;

	InitializeListHead(&subRequestsList);
	rwContext = GetRequestContext(Request);

	if(RequestType == WdfRequestTypeRead)
	{

		rwContext->Read = TRUE;
	}
	else
	{
		rwContext->Read = FALSE;
	}

	USBMSG("%s - begins\n",
	       rwContext->Read ? "Read":"Write");
	if(TotalLength < 8)
	{

		status = STATUS_INVALID_PARAMETER;
		goto Exit;
	}

	deviceContext = GetDeviceContext(WdfIoQueueGetDevice(Queue));

	//
	// Create a collection to store all the sub requests.
	//
	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = Request;
	status = WdfCollectionCreate(&attributes,
	                             &hCollection);
	if (!NT_SUCCESS(status))
	{
		USBERR("WdfCollectionCreate failed. status=%Xh\n", status);
		goto Exit;
	}

	WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
	attributes.ParentObject = hCollection;
	status = WdfSpinLockCreate(&attributes, &hSpinLock);
	if (!NT_SUCCESS(status))
	{
		USBERR("WdfSpinLockCreate failed. status=%Xh\n", status);
		goto Exit;
	}

	//
	// each packet can hold this much info
	//
	packetSize = PipeContext->PipeInformation.MaximumPacketSize;

	numberOfPackets = (TotalLength + packetSize - 1) / packetSize;

	if(0 == (numberOfPackets % 8))
	{

		actualPackets = numberOfPackets;
	}
	else
	{

		//
		// we need multiple of 8 packets only.
		//
		actualPackets = numberOfPackets +
		                (8 - (numberOfPackets % 8));
	}

	minDataInEachPacket = TotalLength / actualPackets;

	if(minDataInEachPacket == packetSize)
	{

		numberOfPacketsFilledToBrim = actualPackets;
		dataLeftToBeDistributed     = 0;

		USBMSG("TotalLength = %d\n", TotalLength);
		USBMSG("PacketSize  = %d\n", packetSize);
		USBMSG("Each of %d packets has %d bytes\n",
		       numberOfPacketsFilledToBrim,
		       packetSize);
	}
	else
	{

		dataLeftToBeDistributed = TotalLength -
		                          (minDataInEachPacket * actualPackets);

		numberOfPacketsFilledToBrim = dataLeftToBeDistributed /
		                              (packetSize - minDataInEachPacket);

		dataLeftToBeDistributed -= (numberOfPacketsFilledToBrim *
		                            (packetSize - minDataInEachPacket));


		USBMSG("TotalLength = %d\n", TotalLength);
		USBMSG("PacketSize  = %d\n", packetSize);
		USBMSG("Each of %d packets has %d bytes\n",
		       numberOfPacketsFilledToBrim,
		       packetSize);
		if(dataLeftToBeDistributed)
		{

			USBMSG("One packet has %d bytes\n",
			       minDataInEachPacket + dataLeftToBeDistributed);
			USBMSG("Each of %d packets has %d bytes\n",
			       actualPackets - (numberOfPacketsFilledToBrim + 1),
			       minDataInEachPacket);
		}
		else
		{
			USBMSG("Each of %d packets has %d bytes\n",
			       actualPackets - numberOfPacketsFilledToBrim,
			       minDataInEachPacket);
		}
	}

	//
	// determine how many stages of transfer needs to be done.
	// in other words, how many irp/urb pairs required.
	// this irp/urb pair is also called the subsidiary irp/urb pair
	//
	numSubRequests = (actualPackets + 1023) / 1024;

	USBMSG("numSubRequests = %d\n", numSubRequests);

	rwContext->SubRequestCollection = hCollection;
	rwContext->SubRequestCollectionLock = hSpinLock;
	rwContext->OriginalTransferMDL = TransferMDL;

	virtualAddress = (PUCHAR) MmGetMdlVirtualAddress(TransferMDL);

	for(i = 0; i < numSubRequests; i++)
	{

		WDFMEMORY               subUrbMemory;
		PURB                    subUrb;
		PMDL                    subMdl;
		ULONG                   nPackets;
		ULONG                   siz;
		ULONG                   offset;


		//
		// For every stage of transfer we need to do the following
		// tasks
		// 1. allocate a request
		// 2. allocate an urb
		// 3. allocate a mdl.
		// 4. Format the request for transfering URB
		// 5. Send the Request.

		//
		if(actualPackets <= 1024)
		{

			nPackets = actualPackets;
			actualPackets = 0;
		}
		else
		{

			nPackets = 1024;
			actualPackets -= 1024;
		}

		USBMSG("nPackets = %d for Irp/URB pair %d\n", nPackets, i);

		ASSERT(nPackets <= 1024);

		siz = GET_ISO_URB_SIZE(nPackets);


		WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
		WDF_OBJECT_ATTRIBUTES_SET_CONTEXT_TYPE(&attributes, SUB_REQUEST_CONTEXT            );


		status = WdfRequestCreate(
		             &attributes,
		             WdfUsbTargetDeviceGetIoTarget(deviceContext->WdfUsbTargetDevice),
		             &subRequest);

		if(!NT_SUCCESS(status))
		{

			USBERR("WdfRequestCreate failed. status=%Xh\n", status);
			goto Exit;
		}

		subReqContext = GetSubRequestContext(subRequest);
		subReqContext->UserRequest = Request;

		//
		// Allocate memory for URB.
		//
		WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
		attributes.ParentObject = subRequest;
		status = WdfMemoryCreate(
		             &attributes,
		             NonPagedPool,
		             POOL_TAG,
		             siz,
		             &subUrbMemory,
		             (PVOID*) &subUrb);

		if (!NT_SUCCESS(status))
		{
			WdfObjectDelete(subRequest);
			USBERR0("Failed to alloc MemoryBuffer for suburb\n");
			goto Exit;
		}

		subReqContext->SubUrb = subUrb;


		if(nPackets > numberOfPacketsFilledToBrim)
		{

			stageSize =  packetSize * numberOfPacketsFilledToBrim;
			stageSize += (minDataInEachPacket *
			              (nPackets - numberOfPacketsFilledToBrim));
			stageSize += dataLeftToBeDistributed;
		}
		else
		{

			stageSize = packetSize * nPackets;
		}

		//
		// allocate a mdl.
		//
		subMdl = IoAllocateMdl((PVOID) virtualAddress,
		                       stageSize,
		                       FALSE,
		                       FALSE,
		                       NULL);

		if(subMdl == NULL)
		{

			USBERR0("failed to alloc mem for sub context mdl\n");
			WdfObjectDelete(subRequest);
			status = STATUS_INSUFFICIENT_RESOURCES;
			goto Exit;
		}

		IoBuildPartialMdl(TransferMDL,
		                  subMdl,
		                  (PVOID) virtualAddress,
		                  stageSize);

		subReqContext->SubMdl = subMdl;

		virtualAddress += stageSize;
		TotalLength -= stageSize;

		usbdPipeHandle = WdfUsbTargetPipeWdmGetPipeHandle(PipeContext->Pipe);
		subUrb->UrbIsochronousTransfer.Hdr.Length = (USHORT) siz;
		subUrb->UrbIsochronousTransfer.Hdr.Function = URB_FUNCTION_ISOCH_TRANSFER;
		subUrb->UrbIsochronousTransfer.PipeHandle = usbdPipeHandle;

		if(rwContext->Read)
		{

			subUrb->UrbIsochronousTransfer.TransferFlags =
			    USBD_TRANSFER_DIRECTION_IN;
		}
		else
		{

			subUrb->UrbIsochronousTransfer.TransferFlags =
			    USBD_TRANSFER_DIRECTION_OUT;
		}

		subUrb->UrbIsochronousTransfer.TransferBufferLength = stageSize;
		subUrb->UrbIsochronousTransfer.TransferBufferMDL = subMdl;
		/*
		        This is a way to set the start frame and NOT specify ASAP flag.

		        status = WdfUsbTargetDeviceRetrieveCurrentFrameNumber(wdfUsbDevice, &frameNumber);
		        subUrb->UrbIsochronousTransfer.StartFrame = frameNumber  + SOME_LATENCY;
		*/
		subUrb->UrbIsochronousTransfer.TransferFlags |=
		    USBD_START_ISO_TRANSFER_ASAP;

		subUrb->UrbIsochronousTransfer.NumberOfPackets = nPackets;
		subUrb->UrbIsochronousTransfer.UrbLink = NULL;

		//
		// set the offsets for every packet for reads/writes
		//
		if(rwContext->Read)
		{

			offset = 0;

			for(j = 0; j < nPackets; j++)
			{

				subUrb->UrbIsochronousTransfer.IsoPacket[j].Offset = offset;
				subUrb->UrbIsochronousTransfer.IsoPacket[j].Length = 0;

				if(numberOfPacketsFilledToBrim)
				{

					offset += packetSize;
					numberOfPacketsFilledToBrim--;
					status = RtlULongSub(stageSize, packetSize, &stageSize);
					ASSERT(NT_SUCCESS(status));
				}
				else if(dataLeftToBeDistributed)
				{

					offset += (minDataInEachPacket + dataLeftToBeDistributed);
					stageSize -= (minDataInEachPacket + dataLeftToBeDistributed);
					dataLeftToBeDistributed = 0;
				}
				else
				{

					offset += minDataInEachPacket;
					stageSize -= minDataInEachPacket;
				}
			}

			ASSERT(stageSize == 0);
		}
		else
		{

			offset = 0;

			for(j = 0; j < nPackets; j++)
			{

				subUrb->UrbIsochronousTransfer.IsoPacket[j].Offset = offset;

				if(numberOfPacketsFilledToBrim)
				{

					subUrb->UrbIsochronousTransfer.IsoPacket[j].Length = packetSize;
					offset += packetSize;
					numberOfPacketsFilledToBrim--;
					stageSize -= packetSize;
				}
				else if(dataLeftToBeDistributed)
				{

					subUrb->UrbIsochronousTransfer.IsoPacket[j].Length =
					    minDataInEachPacket + dataLeftToBeDistributed;
					offset += (minDataInEachPacket + dataLeftToBeDistributed);
					stageSize -= (minDataInEachPacket + dataLeftToBeDistributed);
					dataLeftToBeDistributed = 0;

				}
				else
				{
					subUrb->UrbIsochronousTransfer.IsoPacket[j].Length = minDataInEachPacket;
					offset += minDataInEachPacket;
					stageSize -= minDataInEachPacket;
				}
			}

			ASSERT(stageSize == 0);
		}

		//
		// Associate the URB with the request.
		//
		status = WdfUsbTargetPipeFormatRequestForUrb(PipeContext->Pipe,
		         subRequest,
		         subUrbMemory,
		         NULL );
		if (!NT_SUCCESS(status))
		{
			USBERR0("Failed to format requset for urb\n");
			WdfObjectDelete(subRequest);
			IoFreeMdl(subMdl);
			goto Exit;
		}

		WdfRequestSetCompletionRoutine(subRequest,
		                               SubRequestCompletionRoutine,
		                               rwContext);

		//
		// WdfCollectionAdd takes a reference on the request object and removes
		// it when you call WdfCollectionRemove.
		//
		status = WdfCollectionAdd(hCollection, subRequest);
		if (!NT_SUCCESS(status))
		{
			USBERR("WdfCollectionAdd failed. status=%Xh\n", status);
			WdfObjectDelete(subRequest);
			IoFreeMdl(subMdl);
			goto Exit;
		}

		InsertTailList(&subRequestsList, &subReqContext->ListEntry);

	}

	//
	// There is a subtle race condition which can happen if the cancel
	// routine is running while the sub-request completion routine is
	// trying to mark the request as uncancellable followed by completing
	// the request.
	//
	// We take a reference to prevent the above race condition.
	// The reference is released in the sub-request completion routine
	// if the request wasn't cancelled or in the cancel routine if it was.
	//
	// NOTE: The reference just keeps the request context  from being deleted but
	// if you need to access anything in the WDFREQUEST itself in the cancel or the
	// completion routine make sure that you synchronise the completion of the request with
	// accessing anything in it.
	// You can also use a spinlock  and use that to synchronise
	// the cancel routine with the completion routine.
	//
	WdfObjectReference(Request);
	//
	// Mark the main request cancelable so that we can cancel the subrequests
	// if the main requests gets cancelled for any reason.
	//
	WdfRequestMarkCancelable(Request, LUsbW_EvtRequestCancel);
	cancelable = TRUE;

	while(!IsListEmpty(&subRequestsList))
	{

		thisEntry = RemoveHeadList(&subRequestsList);
		subReqContext = CONTAINING_RECORD(thisEntry, SUB_REQUEST_CONTEXT, ListEntry);
		subRequest = WdfObjectContextGetObject(subReqContext);
		USBMSG("Sending subRequest 0x%p\n", subRequest);

		if (WdfRequestSend(subRequest, WdfUsbTargetPipeGetIoTarget(PipeContext->Pipe), WDF_NO_SEND_OPTIONS) == FALSE)
		{
			status = WdfRequestGetStatus(subRequest);
			//
			// Insert the subrequest back into the subrequestlist so cleanup can find it and delete it
			//
			InsertHeadList(&subRequestsList, &subReqContext->ListEntry);
			USBERR("WdfRequestSend failed. status=%Xh\n", status);
			WdfVerifierDbgBreakPoint(); // break into the debugger if the registry value is set.
			goto Exit;
		}


		//
		// Don't touch the subrequest after it has been sent.
		// Make a note that at least one subrequest is sent. This will be used
		// in deciding whether we should free the subrequests in case of failure.
		//
	}



Exit:

	//
	// Checking the status besides the number of list entries will help differentiate
	// failures where everything succeeded vs where there were failures before adding
	// list entries.
	//
	if(NT_SUCCESS(status) && IsListEmpty(&subRequestsList))
	{
		//
		// We will let the completion routine to cleanup and complete the
		// main request.
		//
		return;
	}
	else
	{
		BOOLEAN  completeRequest;
		NTSTATUS tempStatus;

		completeRequest = TRUE;
		tempStatus = STATUS_SUCCESS;

		if(hCollection)
		{
			DoSubRequestCleanup(rwContext, &subRequestsList, &completeRequest);
		}

		if (completeRequest)
		{
			if (cancelable)
			{
				//
				// Mark the main request as not cancelable before completing it.
				//
				tempStatus = WdfRequestUnmarkCancelable(Request);
				if (NT_SUCCESS(tempStatus))
				{
					//
					// If WdfRequestUnmarkCancelable returns STATUS_SUCCESS
					// that means the cancel routine has been removed. In that case
					// we release the reference otherwise the cancel routine does it.
					//
					WdfObjectDereference(Request);
				}
			}

			if (rwContext->TotalTransferred > 0 )
			{
				WdfRequestCompleteWithInformation(Request, STATUS_SUCCESS, rwContext->TotalTransferred);
			}
			else
			{
				WdfRequestCompleteWithInformation(Request, status, rwContext->TotalTransferred);
			}

		}

	}

	USBMSG0("ends\n");
	return;
}

VOID
DoSubRequestCleanup(
    PREQUEST_CONTEXT    MainRequestContext,
    PLIST_ENTRY         SubRequestsList,
    PBOOLEAN            CompleteRequest
)
{
	PLIST_ENTRY           thisEntry;
	PSUB_REQUEST_CONTEXT  subReqContext;
	WDFREQUEST            subRequest;
	ULONG                 numPendingRequests;

	*CompleteRequest = TRUE;
	WdfSpinLockAcquire(MainRequestContext->SubRequestCollectionLock);

	while(!IsListEmpty(SubRequestsList))
	{
		thisEntry = RemoveHeadList(SubRequestsList);
		subReqContext = CONTAINING_RECORD(thisEntry, SUB_REQUEST_CONTEXT, ListEntry);
		subRequest = WdfObjectContextGetObject(subReqContext);
		WdfCollectionRemove(MainRequestContext->SubRequestCollection, subRequest);

		if(subReqContext->SubMdl)
		{
			IoFreeMdl(subReqContext->SubMdl);
			subReqContext->SubMdl = NULL;
		}

		WdfObjectDelete(subRequest);
	}
	numPendingRequests = WdfCollectionGetCount(MainRequestContext->SubRequestCollection);
	WdfSpinLockRelease(MainRequestContext->SubRequestCollectionLock);

	if (numPendingRequests > 0)
	{
		*CompleteRequest = FALSE;
	}

	return;
}

VOID
SubRequestCompletionRoutine(
    IN WDFREQUEST                  Request,
    IN WDFIOTARGET                 Target,
    PWDF_REQUEST_COMPLETION_PARAMS CompletionParams,
    IN WDFCONTEXT                  Context
)
/*++

Routine Description:

    Completion Routine

Arguments:

    Context - Driver supplied context
    Target - Target handle
    Request - Request handle
    Params - request completion params


Return Value:

    VOID

--*/
{
	PURB                    urb;
	ULONG                   i;
	ULONG                   numPendingRequests;
	NTSTATUS                status;
	PREQUEST_CONTEXT        rwContext;
	WDFREQUEST              mainRequest;
	PSUB_REQUEST_CONTEXT    subReqContext;

	UNREFERENCED_PARAMETER(Target);

	USBMSG0("begins\n");

	subReqContext = GetSubRequestContext(Request);

	urb = (PURB) subReqContext->SubUrb;

	IoFreeMdl(subReqContext->SubMdl);
	subReqContext->SubMdl = NULL;

	rwContext = (PREQUEST_CONTEXT) Context;
	ASSERT(rwContext);

	status = CompletionParams->IoStatus.Status;

	if(NT_SUCCESS(status) &&
	        USBD_SUCCESS(urb->UrbHeader.Status))
	{

		rwContext->TotalTransferred +=
		    urb->UrbIsochronousTransfer.TransferBufferLength;

		USBMSG("rwContext->TotalTransferred = %d\n", rwContext->TotalTransferred);
	}
	else
	{

		USBERR("read-write irp failed. status=%Xh\n", status);
		USBERR("urb header status=%Xh\n", urb->UrbHeader.Status);
	}

	for(i = 0; i < urb->UrbIsochronousTransfer.NumberOfPackets; i++)
	{

		USBDBG("IsoPacket[%d].Length = %X IsoPacket[%d].Status = %X\n",
		       i,
		       urb->UrbIsochronousTransfer.IsoPacket[i].Length,
		       i,
		       urb->UrbIsochronousTransfer.IsoPacket[i].Status);
	}

	//
	// Remove the SubRequest from the collection.
	//
	WdfSpinLockAcquire(rwContext->SubRequestCollectionLock);

	WdfCollectionRemove(rwContext->SubRequestCollection, Request);

	numPendingRequests = WdfCollectionGetCount(rwContext->SubRequestCollection);

	WdfSpinLockRelease(rwContext->SubRequestCollectionLock);

	//
	// If all the sub requests are completed. Complete the main request sent
	// by the user application.
	//
	if(numPendingRequests == 0)
	{

		USBMSG0("no more pending sub requests\n");

		if(NT_SUCCESS(status))
		{

			USBMSG("urb start frame %X\n",
			       urb->UrbIsochronousTransfer.StartFrame);
		}

		mainRequest = WdfObjectContextGetObject(rwContext);

		//
		// if we transferred some data, main Irp completes with success
		//
		USBMSG("Total data transferred = %X\n", rwContext->TotalTransferred);

		USBMSG("SubRequestCompletionRoutine %s completed\n",
		       rwContext->Read?"Read":"Write");
		//
		// Mark the main request as not cancelable before completing it.
		//
		status = WdfRequestUnmarkCancelable(mainRequest);
		if (NT_SUCCESS(status))
		{
			//
			// If WdfRequestUnmarkCancelable returns STATUS_SUCCESS
			// that means the cancel routine has been removed. In that case
			// we release the reference otherwise the cancel routine does it.
			//
			WdfObjectDereference(mainRequest);
		}

		if (rwContext->TotalTransferred > 0 )
		{
			WdfRequestCompleteWithInformation(mainRequest, STATUS_SUCCESS, rwContext->TotalTransferred);
		}
		else
		{
			USBMSG("SubRequestCompletionRoutine completiong with failure status %x\n",
			       CompletionParams->IoStatus.Status);
			WdfRequestCompleteWithInformation(mainRequest, CompletionParams->IoStatus.Status, rwContext->TotalTransferred);
		}

	}

	//
	// Since we created the subrequests, we should free it by removing the
	// reference.
	//
	WdfObjectDelete(Request);

	USBMSG0("ends\n");

	return;
}

VOID
LUsbW_EvtRequestCancel(
    WDFREQUEST Request
)
/*++

Routine Description:

    This is the cancellation routine for the main read/write Irp.

Arguments:


Return Value:

    None

--*/
{
	PREQUEST_CONTEXT        rwContext;
	LIST_ENTRY              cancelList;
	PSUB_REQUEST_CONTEXT    subReqContext;
	WDFREQUEST              subRequest;
	ULONG                   i;
	PLIST_ENTRY             thisEntry;

	USBMSG0("begins\n");

	//
	// In this case since the cancel routine just references the request context, a reference
	// on the WDFREQUEST is enough. If it needed to access the underlying WDFREQUEST  to get the
	// underlying IRP etc. consider using a spinlock for synchronisation with the request
	// completion routine.
	//
	rwContext = GetRequestContext(Request);

	if(!rwContext->SubRequestCollection)
	{
		ASSERTMSG("Very unlikely, collection is created before\
                        the request is made cancellable", FALSE);
		return;
	}

	InitializeListHead(&cancelList);

	//
	// We cannot call the WdfRequestCancelSentRequest with the collection lock
	// acquired because that call can recurse into our completion routine,
	// when the lower driver completes the request in the CancelRoutine,
	// and can cause deadlock when we acquire the collection to remove the
	// subrequest. So to workaround that, we will get the item from the
	// collection, take an extra reference, and put them in the local list.
	// Then we drop the lock, walk the list and call WdfRequestCancelSentRequest and
	// remove the extra reference.
	//
	WdfSpinLockAcquire(rwContext->SubRequestCollectionLock);

	for(i=0; i<WdfCollectionGetCount(rwContext->SubRequestCollection); i++)
	{


		subRequest = WdfCollectionGetItem(rwContext->SubRequestCollection, i);

		subReqContext = GetSubRequestContext(subRequest);

		WdfObjectReference(subRequest);

		InsertTailList(&cancelList, &subReqContext->ListEntry);
	}

	WdfSpinLockRelease(rwContext->SubRequestCollectionLock);

	while(!IsListEmpty(&cancelList))
	{
		thisEntry = RemoveHeadList(&cancelList);

		subReqContext = CONTAINING_RECORD(thisEntry, SUB_REQUEST_CONTEXT, ListEntry);

		subRequest = WdfObjectContextGetObject(subReqContext);

		USBMSG("Cancelling subRequest 0x%p\n", subRequest);

		if(!WdfRequestCancelSentRequest(subRequest))
		{
			USBMSG0("WdfRequestCancelSentRequest failed\n");
		}

		WdfObjectDereference(subRequest);
	}

	//
	// Release the reference we took earlier on the main request.
	//
	WdfObjectDereference(Request);
	USBMSG0("ends\n");

	return;
}

VOID
LUsbWEvtIoStop(
    __in WDFQUEUE         Queue,
    __in WDFREQUEST       Request,
    __in ULONG            ActionFlags
)
/*++

Routine Description:

    This callback is invoked on every inflight request when the device
    is suspended or removed. Since our inflight read and write requests
    are actually pending in the target device, we will just acknowledge
    its presence. Until we acknowledge, complete, or requeue the requests
    framework will wait before allowing the device suspend or remove to
    proceeed. When the underlying USB stack gets the request to suspend or
    remove, it will fail all the pending requests.

Arguments:

Return Value:
    None

--*/
{
	PREQUEST_CONTEXT reqContext;

	UNREFERENCED_PARAMETER(Queue);


	reqContext=GetRequestContext(Request);

	if (ActionFlags & WdfRequestStopActionSuspend )
	{
		WdfRequestStopAcknowledge(Request, FALSE); // Don't requeue
	}
	else if(ActionFlags & WdfRequestStopActionPurge)
	{
		WdfRequestCancelSentRequest(Request);
	}
	return;
}

VOID
LUsbWEvtIoResume(
    IN WDFQUEUE   Queue,
    IN WDFREQUEST Request
)
/*++

Routine Description:

     This callback is invoked for every request pending in the driver - in-flight
     request - to notify that the hardware is ready for contiuing the processing
     of the request.

Arguments:

    Queue - Queue the request currently belongs to
    Request - Request that is currently out of queue and being processed by the driver

Return Value:

    None.

--*/
{
	UNREFERENCED_PARAMETER(Queue);
	UNREFERENCED_PARAMETER(Request);
	// Leave this function here for now in case we want to add anything in the future
}
