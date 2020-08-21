/****************************************************************************
**
** Copyright (C) 2019 BlackINT3
** Contact: https://github.com/BlackINT3/OpenArk
**
** GNU Lesser General Public License Usage (LGPL)
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file. Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
****************************************************************************/
#include "../arkdrv-api/arkdrv-api.h"
#include "../common/common.h"
#include "memory.h"

#ifndef MM_COPY_MEMORY_VIRTUAL
#define MM_COPY_MEMORY_VIRTUAL 0x2
#endif
#ifndef _MM_COPY_ADDRESS
typedef struct _MM_COPY_ADDRESS {
	union {
		PVOID VirtualAddress;
		PHYSICAL_ADDRESS PhysicalAddress;
	};
} MM_COPY_ADDRESS, *PMMCOPY_ADDRESS;
#endif
typedef NTSTATUS(NTAPI *__MmCopyMemory)(
	PVOID TargetAddress,
	MM_COPY_ADDRESS SourceAddress,
	SIZE_T NumberOfBytes,
	ULONG Flags,
	PSIZE_T NumberOfBytesTransferred
);
#define ARK_MEMROY_HEADER_SIZE (sizeof(ARK_MEMORY_OUT) - sizeof(UCHAR))

BOOLEAN MmReadKernelMemory(PVOID addr, PVOID buf, ULONG len)
{
	BOOLEAN ret = FALSE;
	if (addr <= MM_HIGHEST_USER_ADDRESS) return FALSE;

	if (ArkDrv.ver >= NTOS_WIN81) {
		PVOID data = ExAllocatePool(NonPagedPool, len);
		if (data) {
			auto pMmCopyMemory = (__MmCopyMemory)GetNtRoutineAddress(L"MmCopyMemory");
			if (pMmCopyMemory) {
				SIZE_T cplen;
				MM_COPY_ADDRESS cpaddr;
				cpaddr.VirtualAddress = addr;
				NTSTATUS status = pMmCopyMemory(data, cpaddr, len, MM_COPY_MEMORY_VIRTUAL, &cplen);
				if (NT_SUCCESS(status)) {
					RtlCopyMemory(buf, data, cplen);
					ret = TRUE;
				}
			}
			ExFreePool(data);
		}
		return ret;
	}

	// [TDOO] BYTE_OFFSET PAGE_ALIGN
	PHYSICAL_ADDRESS pa;
	pa = MmGetPhysicalAddress(addr);
	if (pa.QuadPart) {
		PVOID va = MmMapIoSpace(pa, len, MmNonCached);
		if (va) {
			RtlCopyMemory(buf, va, len);
			MmUnmapIoSpace(va, len);
			ret = TRUE;
		}
	}
	return ret;
}

BOOLEAN MmWriteKernelMemory(PVOID addr, PVOID buf, ULONG len)
{
	BOOLEAN ret = FALSE;
	if (addr <= MM_HIGHEST_USER_ADDRESS) return FALSE;
	if (!MmIsAddressValid(addr)) return FALSE;

	KIRQL irql = MmWriteProtectOff();
	RtlCopyMemory(addr, buf, len);
	MmWriteProtectOn(irql);

	return TRUE;
}

NTSTATUS MemoryReadData(PARK_MEMORY_IN inbuf, ULONG inlen, PVOID outbuf, ULONG outlen, PIRP irp)
{
	ULONG size = ARK_MEMROY_HEADER_SIZE + inbuf->size;
	if (size > outlen) {
		irp->IoStatus.Information = size;
		return STATUS_BUFFER_OVERFLOW;
	}
	PVOID data = ExAllocatePool(NonPagedPool, size);
	if (!data) return STATUS_MEMORY_NOT_ALLOCATED;

	BOOL attach = FALSE;
	PEPROCESS eproc = NULL;
	KAPC_STATE apc_state;
	ULONG pid = inbuf->pid;
	if (pid != 4 && pid != 0 && pid != (ULONG)PsGetCurrentProcessId()) {
		if (NT_SUCCESS(PsLookupProcessByProcessId((HANDLE)pid, &eproc))) {
			KeStackAttachProcess(eproc, &apc_state);
			attach = TRUE;
		}
	}
	BOOLEAN ret = MmReadKernelMemory((PVOID)inbuf->addr, data, size);
	if (attach) {
		KeUnstackDetachProcess(&apc_state);
		ObDereferenceObject(eproc);
	}
	if (!ret) {
		ExFreePool(data);
		return STATUS_UNSUCCESSFUL;
	}
	PARK_MEMORY_OUT memout = (PARK_MEMORY_OUT)outbuf;
	memout->size = size;
	memout->pid = pid;
	RtlCopyMemory(memout->readbuf, data, size);
	ExFreePool(data);
	irp->IoStatus.Information = ARK_MEMROY_HEADER_SIZE + size;
	return STATUS_SUCCESS;
}

NTSTATUS MemoryWriteData(PARK_MEMORY_IN inbuf, ULONG inlen, PVOID outbuf, ULONG outlen, PIRP irp)
{
	ULONG size = ARK_MEMROY_HEADER_SIZE + inbuf->size;
	if (size > outlen) {
		irp->IoStatus.Information = size;
		return STATUS_BUFFER_OVERFLOW;
	}

	BOOL attach = FALSE;
	PEPROCESS eproc = NULL;
	KAPC_STATE apc_state;
	ULONG pid = inbuf->pid;
	if (pid != 4 && pid != 0 && pid != (ULONG)PsGetCurrentProcessId()) {
		if (NT_SUCCESS(PsLookupProcessByProcessId((HANDLE)pid, &eproc))) {
			KeStackAttachProcess(eproc, &apc_state);
			attach = TRUE;
		}
	}
	BOOLEAN ret = MmWriteKernelMemory((PVOID)inbuf->addr, inbuf->u.writebuf, size);
	if (attach) {
		KeUnstackDetachProcess(&apc_state);
		ObDereferenceObject(eproc);
	}
	if (!ret) {
		return STATUS_UNSUCCESSFUL;
	}
	PARK_MEMORY_OUT memout = (PARK_MEMORY_OUT)outbuf;
	memout->pid = pid;
	irp->IoStatus.Information = ARK_MEMROY_HEADER_SIZE;
	return STATUS_SUCCESS;
}

NTSTATUS MemoryDispatcher(IN ULONG op, IN PDEVICE_OBJECT devobj, PVOID inbuf, ULONG inlen, PVOID outbuf, ULONG outlen, IN PIRP irp)
{
	NTSTATUS status = STATUS_UNSUCCESSFUL;
	switch (op) {
	case ARK_MEMORY_READ:
		status = MemoryReadData((PARK_MEMORY_IN)inbuf, inlen, outbuf, outlen, irp);
		break;
	case ARK_MEMORY_WRITE:
		status = MemoryWriteData((PARK_MEMORY_IN)inbuf, inlen, outbuf, outlen, irp);
		break;
	default:
		break;
	}

	return status;
}