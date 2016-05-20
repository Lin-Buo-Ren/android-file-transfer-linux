/*
    This file is part of Android File Transfer For Linux.
    Copyright (C) 2015-2016  Vladimir Menshakov

    Android File Transfer For Linux is free software: you can redistribute
    it and/or modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation, either version 3 of the
    License, or (at your option) any later version.

    Android File Transfer For Linux is distributed in the hope that it will
    be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
    of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Android File Transfer For Linux.
    If not, see <http://www.gnu.org/licenses/>.
 */

#include <usb/Device.h>
#include <Exception.h>
#include <mtp/usb/TimeoutException.h>
#include <mtp/usb/DeviceBusyException.h>
#include <mtp/usb/DeviceNotFoundException.h>
#include <mtp/ByteArray.h>
#include <mtp/log.h>

#include <sys/ioctl.h>
#include <sys/time.h>
#include <poll.h>
#include <signal.h>

#include "linux/usbdevice_fs.h"

#define IOCTL(...) do \
{ \
	int r = ioctl(__VA_ARGS__); \
	if (r < 0) \
	{ \
		if (errno == EBUSY) \
			throw DeviceBusyException(); \
		else if (errno == ENODEV) \
			throw DeviceNotFoundException(); \
		else \
			throw posix::Exception("ioctl(" #__VA_ARGS__ ")"); \
	} \
} while(false)

namespace mtp { namespace usb
{

	InterfaceToken::InterfaceToken(int fd, unsigned interfaceNumber): _fd(fd), _interfaceNumber(interfaceNumber)
	{
		IOCTL(_fd, USBDEVFS_CLAIMINTERFACE, &interfaceNumber);
	}

	InterfaceToken::~InterfaceToken()
	{
		ioctl(_fd, USBDEVFS_RELEASEINTERFACE, &_interfaceNumber);
	}

#define PRINT_CAP(CAP, NAME) \
	if (capabilities & (CAP)) \
	{ \
		debug(NAME " "); \
		capabilities &= ~(CAP); \
	}


	Device::Device(int fd, const EndpointPtr &controlEp): _fd(fd), _capabilities(0), _controlEp(controlEp)
	{
		try { IOCTL(_fd.Get(), USBDEVFS_GET_CAPABILITIES, &_capabilities); }
		catch(const std::exception &ex)
		{ error("get usbfs capabilities failed: ", ex.what()); }
		debug("capabilities = 0x", hex(_capabilities, 8));
		if (_capabilities)
		{
			u32 capabilities = _capabilities;
			PRINT_CAP(USBDEVFS_CAP_ZERO_PACKET, "<zero>");
			PRINT_CAP(USBDEVFS_CAP_BULK_CONTINUATION, "<bulk-continuation>");
			PRINT_CAP(USBDEVFS_CAP_NO_PACKET_SIZE_LIM, "<no-packet-size-limit>");
			PRINT_CAP(USBDEVFS_CAP_BULK_SCATTER_GATHER, "<bulk-scatter-gather>");
			PRINT_CAP(USBDEVFS_CAP_REAP_AFTER_DISCONNECT, "<reap-after-disconnect>");
			if (capabilities)
				debug("<unknown capability 0x", hex(capabilities, 2), ">");
		}
		else
			debug("[none]\n");
	}

	Device::~Device()
	{ }

	int Device::GetConfiguration() const
	{
		return 0;
	}

	void Device::SetConfiguration(int idx)
	{
		error("SetConfiguration(", idx, "): not implemented");
	}

	struct Device::Urb : Noncopyable
	{
		static const int 		MaxBufferSize = 4096;
		int						Fd;
		int						PacketSize;
		ByteArray				Buffer;
		usbdevfs_urb			KernelUrb;

		Urb(int fd, u8 type, const EndpointPtr & ep): Fd(fd), PacketSize(ep->GetMaxPacketSize()), Buffer(std::max(PacketSize, MaxBufferSize / PacketSize * PacketSize)), KernelUrb()
		{
			KernelUrb.type			= type;
			KernelUrb.endpoint		= ep->GetAddress();
			KernelUrb.buffer		= Buffer.data();
			KernelUrb.buffer_length = Buffer.size();
		}

		size_t GetTransferSize() const
		{ return Buffer.size(); }

		void Submit()
		{
			IOCTL(Fd, USBDEVFS_SUBMITURB, &KernelUrb);
		}

		void Discard()
		{
			int r = ioctl(Fd, USBDEVFS_DISCARDURB, &KernelUrb);
			if (r != 0)
			{
				perror("ioctl(USBDEVFS_DISCARDURB)");
			}
		}

		size_t Send(const IObjectInputStreamPtr &inputStream, size_t size)
		{
			size_t r = inputStream->Read(Buffer.data(), size);
			//HexDump("write", ByteArray(Buffer.data(), Buffer.data() + r), true);
			KernelUrb.buffer_length = r;
			return r;
		}

		size_t Send(const ByteArray &inputData)
		{
			size_t r = std::min(Buffer.size(), inputData.size());
			std::copy(inputData.data(), inputData.data() + r, Buffer.data());
			KernelUrb.buffer_length = r;
			return r;
		}

		size_t Recv(const IObjectOutputStreamPtr &outputStream)
		{
			//HexDump("read", ByteArray(Buffer.data(), Buffer.data() + KernelUrb.actual_length), true);
			return outputStream->Write(Buffer.data(), KernelUrb.actual_length);
		}

		ByteArray Recv()
		{ return ByteArray(Buffer.begin(), Buffer.begin() + KernelUrb.actual_length); }

		template<unsigned Flag>
		void SetFlag(bool value)
		{
			if (value)
				KernelUrb.flags |= Flag;
			else
				KernelUrb.flags &= ~Flag;
		}

		void SetContinuationFlag(bool continuation)
		{ SetFlag<USBDEVFS_URB_BULK_CONTINUATION>(continuation); }

		void SetZeroPacketFlag(bool zero)
		{ SetFlag<USBDEVFS_URB_ZERO_PACKET>(zero); }
	};

	void * Device::Reap(int timeout)
	{
		timeval started = {};
		if (gettimeofday(&started, NULL) == -1)
			throw posix::Exception("gettimeofday");

		pollfd fd = {};
		fd.fd		= _fd.Get();
		fd.events	= POLLOUT;
		int r = poll(&fd, 1, timeout);

		timeval now = {};
		if (gettimeofday(&now, NULL) == -1)
			throw posix::Exception("gettimeofday");

		if (r < 0)
			throw posix::Exception("poll");

		if (r == 0 && timeout > 0)
		{
			int ms = (now.tv_sec - started.tv_sec) * 1000 + (now.tv_usec - started.tv_usec) / 1000;
			error(ms, " ms since the last poll call");
		}

		usbdevfs_urb *urb;
		r = ioctl(_fd.Get(), USBDEVFS_REAPURBNDELAY, &urb);
		if (r == 0)
			return urb;
		else if (errno == EAGAIN)
			throw TimeoutException("timeout reaping usb urb");
		else
			throw posix::Exception("ioctl");
	}

	void Device::ClearHalt(const EndpointPtr & ep)
	{
		try
		{ unsigned index = ep->GetAddress(); IOCTL(_fd.Get(), USBDEVFS_CLEAR_HALT, &index); }
		catch(const std::exception &ex)
		{ error("clearing halt status for ep ", hex(ep->GetAddress(), 2), ": ", ex.what()); }
	}


	void Device::Submit(const UrbPtr &urb, int timeout)
	{
		urb->Submit();
		{
			scoped_mutex_lock l(_mutex);
			_urbs.insert(std::make_pair(&urb->KernelUrb, urb));
		}
		try
		{
			while(true)
			{
				UrbPtr completedUrb;
				{
					void *completedKernelUrb = Reap(timeout);
					scoped_mutex_lock l(_mutex);
					auto urbIt = _urbs.find(completedKernelUrb);
					if (urbIt == _urbs.end())
					{
						error("got unknown urb: ", completedKernelUrb);
						continue;
					}
					completedUrb = urbIt->second;
					_urbs.erase(urbIt);
				}
				if (completedUrb == urb)
					break;
			}
		}
		catch(const TimeoutException &ex)
		{
			urb->Discard();
			throw;
		}
		catch(const std::exception &ex)
		{
			error("error while submitting urb: ", ex.what());
			urb->Discard();
			throw;
		}
	}

	void Device::WriteBulk(const EndpointPtr & ep, const IObjectInputStreamPtr &inputStream, int timeout)
	{
		UrbPtr urb = std::make_shared<Urb>(_fd.Get(), USBDEVFS_URB_TYPE_BULK, ep);
		size_t transferSize = urb->GetTransferSize();

		size_t r;
		bool continuation = false;
		do
		{
			r = urb->Send(inputStream, transferSize);

			if (_capabilities & USBDEVFS_CAP_ZERO_PACKET)
				urb->SetZeroPacketFlag(r != transferSize);

			if (_capabilities & USBDEVFS_CAP_BULK_CONTINUATION)
			{
				urb->SetContinuationFlag(continuation);
				continuation = true;
			}
			Submit(urb, timeout);
		}
		while(r == transferSize);
	}

	void Device::ReadBulk(const EndpointPtr & ep, const IObjectOutputStreamPtr &outputStream, int timeout)
	{
		UrbPtr urb = std::make_shared<Urb>(_fd.Get(), USBDEVFS_URB_TYPE_BULK, ep);
		size_t transferSize = urb->GetTransferSize();

		size_t r;
		bool continuation = false;
		do
		{
			if (_capabilities & USBDEVFS_CAP_BULK_CONTINUATION)
			{
				urb->SetContinuationFlag(continuation);
				continuation = true;
			}
			Submit(urb, timeout);

			r = urb->Recv(outputStream);
		}
		while(r == transferSize);
	}

	u8 Device::TransactionType(const EndpointPtr &ep)
	{
		EndpointType type = ep->GetType();
		switch(type)
		{
		case EndpointType::Control:
			return USBDEVFS_URB_TYPE_CONTROL;
		case EndpointType::Isochronous:
			return USBDEVFS_URB_TYPE_ISO;
		case EndpointType::Bulk:
			return USBDEVFS_URB_TYPE_BULK;
		case EndpointType::Interrupt:
			return USBDEVFS_URB_TYPE_INTERRUPT;
		default:
			throw std::runtime_error("invalid endpoint type");
		}
	}

	void Device::ReadControl(u8 type, u8 req, u16 value, u16 index, ByteArray &data, int timeout)
	{
		debug("read control ", hex(type, 2), " ", hex(req, 2), " ", hex(value, 4), " ", hex(index, 4));
		usbdevfs_ctrltransfer ctrl = { };
		ctrl.bRequestType = type;
		ctrl.bRequest = req;
		ctrl.wValue = value;
		ctrl.wIndex = index;
		ctrl.wLength = data.size();
		ctrl.data = const_cast<u8 *>(data.data());
		ctrl.timeout = timeout;

		int fd = _fd.Get();

		int r = ioctl(fd, USBDEVFS_CONTROL, &ctrl);
		if (r >= 0)
			data.resize(r);
		else if (errno == EAGAIN)
			throw TimeoutException("timeout sending control transfer");
		else
			throw posix::Exception("ioctl");
	}

	void Device::WriteControl(u8 type, u8 req, u16 value, u16 index, const ByteArray &data, int timeout)
	{
		debug("write control ", hex(type, 2), " ", hex(req, 2), " ", hex(value, 4), " ", hex(index, 4));
		usbdevfs_ctrltransfer ctrl = { };
		ctrl.bRequestType = type;
		ctrl.bRequest = req;
		ctrl.wValue = value;
		ctrl.wIndex = index;
		ctrl.wLength = data.size();
		ctrl.data = const_cast<u8 *>(data.data());
		ctrl.timeout = timeout;

		int fd = _fd.Get();

		int r = ioctl(fd, USBDEVFS_CONTROL, &ctrl);
		if (r >= 0)
			return;
		else if (errno == EAGAIN)
			throw TimeoutException("timeout sending control transfer");
		else
			throw posix::Exception("ioctl");
	}

}}
