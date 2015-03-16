/*
 * Copyright (c) 2015, Christian Menard
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are those
 * of the authors and should not be interpreted as representing official policies,
 * either expressed or implied, of the FreeBSD Project.
 */

#include "dtu.hh"

Dtu::Dtu(const DtuParams *p)
  : MemObject(p),
    baseAddr(p->base_addr),
    cpuSideMaster("cpu_side_master", *this),
    cpuSideSlave("cpu_side_slave", *this)
{ }

void
Dtu::init()
{
    MemObject::init();

    assert(cpuSideMaster.isConnected());
    assert(cpuSideSlave.isConnected());

    cpuSideSlave.sendRangeChange();
}

BaseMasterPort&
Dtu::getMasterPort(const std::string &if_name, PortID idx)
{
    if (if_name != "cpu_side_master") {
        return MemObject::getMasterPort(if_name, idx);
    } else {
        return cpuSideMaster;
    }
}

BaseSlavePort&
Dtu::getSlavePort(const std::string &if_name, PortID idx)
{
    if (if_name != "cpu_side_slave") {
        return MemObject::getSlavePort(if_name, idx);
    } else {
        return cpuSideSlave;
    }
}

bool
Dtu::DtuMasterPort::recvTimingResp(PacketPtr pkt)
{
    panic("Did not expect a TimingResp!");
    return true;
}

void
Dtu::DtuMasterPort::recvReqRetry()
{
    panic("Did not expect a ReqRetry!");
}

AddrRangeList
Dtu::DtuSlavePort::getAddrRanges() const
{
    AddrRangeList ranges;
    auto range = AddrRange(dtu.baseAddr, dtu.baseAddr + dtu.size - 1);
    ranges.push_back(range);
    return ranges;
}

Tick
Dtu::DtuSlavePort::recvAtomic(PacketPtr pkt)
{
    panic("Dtu::recvAtomic() not yet implemented");
    return 0;
}

void
Dtu::DtuSlavePort::recvFunctional(PacketPtr pkt)
{
    panic("Dtu::recvFunctional() not yet implemented");
}

bool
Dtu::DtuSlavePort::recvTimingReq(PacketPtr pkt)
{
    panic("Dtu::recvTimingReq() not yet implemented");
}

void
Dtu::DtuSlavePort::recvRespRetry()
{
    panic("Dtu::recvRespRetry() not yet implemented");
}

Dtu*
DtuParams::create()
{
    return new Dtu(this);
}
