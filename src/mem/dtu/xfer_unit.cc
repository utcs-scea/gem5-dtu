/*
 * Copyright (c) 2015, Christian Menard
 * Copyright (c) 2015, Nils Asmussen
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * The views and conclusions contained in the software and documentation are
 * those of the authors and should not be interpreted as representing official
 * policies, either expressed or implied, of the FreeBSD Project.
 */

#include "debug/Dtu.hh"
#include "debug/DtuBuf.hh"
#include "debug/DtuPackets.hh"
#include "debug/DtuSysCalls.hh"
#include "debug/DtuXfers.hh"
#include "mem/dtu/xfer_unit.hh"

uint64_t XferUnit::TransferEvent::nextId = 0;

XferUnit::XferUnit(Dtu &_dtu,
                   size_t _blockSize,
                   size_t _bufCount,
                   size_t _bufSize)
    : dtu(_dtu),
      blockSize(_blockSize),
      bufCount(_bufCount),
      bufSize(_bufSize),
      bufs(new Buffer*[bufCount]),
      abortReqs(),
      queue()
{
    panic_if(dtu.tlb() && bufCount < 2,
        "With paging enabled, at least 2 buffers are required");

    for (size_t i = 0; i < bufCount; ++i)
        bufs[i] = new Buffer(i, bufSize);
}

XferUnit::~XferUnit()
{
    for (size_t i = 0; i < bufCount; ++i)
        delete bufs[i];
    delete[] bufs;
}

void
XferUnit::regStats()
{
    reads
        .init(8)
        .name(dtu.name() + ".xfer.reads")
        .desc("Read times (in Cycles)")
        .flags(Stats::nozero);
    writes
        .init(8)
        .name(dtu.name() + ".xfer.writes")
        .desc("Write times (in Cycles)")
        .flags(Stats::nozero);
    bytesRead
        .init(8)
        .name(dtu.name() + ".xfer.bytesRead")
        .desc("Read bytes (from internal memory)")
        .flags(Stats::nozero);
    bytesWritten
        .init(8)
        .name(dtu.name() + ".xfer.bytesWritten")
        .desc("Written bytes (to internal memory)")
        .flags(Stats::nozero);
    delays
        .name(dtu.name() + ".xfer.delays")
        .desc("Number of delays due to occupied buffers");
    pagefaults
        .name(dtu.name() + ".xfer.pagefaults")
        .desc("Number of pagefaults during transfers");
    aborts
        .name(dtu.name() + ".xfer.aborts")
        .desc("Number of aborts");
}

void
XferUnit::Translation::abort()
{
    event.xfer->dtu.abortTranslate(this);
}

void
XferUnit::Translation::finished(bool success, const NocAddr &phys)
{
    event.translateDone(success, phys);

    delete this;
}

void
XferUnit::TransferEvent::tryStart()
{
    assert(buf == NULL);

    buf = xfer->allocateBuf(this, flags());

    // try again later, if there is no free buffer
    if (!buf)
    {
        DPRINTFS(DtuXfers, (&xfer->dtu),
            "Delaying %s transfer of %lu bytes @ %p [flags=%#x]\n",
            isWrite() ? "mem-write" : "mem-read",
            remaining,
            localAddr(),
            flags());

        xfer->delays++;
        xfer->queue.push_back(this);
        return;
    }

    transferStart();

    DPRINTFS(DtuXfers, (&xfer->dtu),
        "buf%d: Starting %s transfer of %lu bytes @ %p [flags=%#x]\n",
        buf->id,
        isWrite() ? "mem-write" : "mem-read",
        remaining,
        localAddr(),
        flags());

    // should we abort the next request from this core?
    // actually, we could do that earlier, but doing it as soon as we have
    // a buffer, makes it much easier
    auto it = std::find(xfer->abortReqs.begin(),
                        xfer->abortReqs.end(),
                        senderCore());
    if (it != xfer->abortReqs.end())
    {
        abort(Dtu::Error::ABORT);
        xfer->abortReqs.erase(it);
        return;
    }

    xfer->dtu.schedule(this, xfer->dtu.clockEdge(Cycles(1)));
}

void
XferUnit::TransferEvent::process()
{
    if (!buf)
    {
        tryStart();
        return;
    }

    NocAddr phys(localAddr());
    if (xfer->dtu.tlb() && !(flags() & NOXLATE))
    {
        uint access = isWrite() ? DtuTlb::WRITE : DtuTlb::READ;
        access |= isRemote() ? 0 : DtuTlb::INTERN;

        DtuTlb::Result res = xfer->dtu.tlb()->lookup(localAddr(), access, &phys);
        if (res != DtuTlb::HIT)
        {
            if (res == DtuTlb::PAGEFAULT)
                xfer->pagefaults++;

            // if this is a pagefault and we are not allowed to cause one,
            // report an error
            if (res == DtuTlb::PAGEFAULT && (flags() & XferFlags::NOPF))
            {
                abort(Dtu::Error::PAGEFAULT);
                return;
            }

            assert(res != DtuTlb::NOMAP);
            trans = new Translation(*this);
            xfer->dtu.startTranslate(localAddr(), access, trans);
            return;
        }
    }

    translateDone(true, phys);
}

void
XferUnit::TransferEvent::translateDone(bool success, const NocAddr &phys)
{
    // if there was an error, we have aborted it on purpose
    // in this case, TransferEvent::abort() will do the rest
    if (result != Dtu::Error::NONE)
        return;

    trans = NULL;

    if (!success)
    {
        abort(Dtu::Error::PAGEFAULT);
        return;
    }

    assert(remaining > 0);

    Addr localOff = localAddr() & (xfer->blockSize - 1);
    Addr reqSize = std::min(remaining, xfer->blockSize - localOff);

    auto cmd = isWrite() ? MemCmd::WriteReq : MemCmd::ReadReq;
    auto pkt = xfer->dtu.generateRequest(phys.getAddr(), reqSize, cmd);

    if (isWrite())
    {
        assert(buf->offset + reqSize <= xfer->bufSize);

        memcpy(pkt->getPtr<uint8_t>(), buf->bytes + buf->offset, reqSize);

        buf->offset += reqSize;
    }

    DPRINTFS(DtuXfers, (&xfer->dtu),
        "buf%d: %s %lu bytes @ %p->%p in local memory\n",
        buf->id,
        isWrite() ? "Writing" : "Reading",
        reqSize,
        localAddr(),
        phys.getAddr());

    xfer->dtu.sendMemRequest(pkt,
                            localAddr(),
                            id,
                            Dtu::MemReqType::TRANSFER,
                            xfer->dtu.transferToMemRequestLatency);

    // to next block
    local += reqSize;
    remaining -= reqSize;
}

void
XferUnit::TransferEvent::abort(Dtu::Error error)
{
    DPRINTFS(DtuXfers, (&xfer->dtu),
        "buf%d: aborting transfer (%d)\n",
        buf->id,
        static_cast<int>(error));

    buf->event->result = error;
    if (trans)
    {
        trans->abort();
        trans = NULL;
        // will be deleted by abort()
    }

    xfer->aborts++;

    if(scheduled())
        xfer->dtu.deschedule(this);

    buf->event->remaining = 0;
    xfer->recvMemResponse(id, NULL, 0);
}

void
XferUnit::startTransfer(TransferEvent *event, Cycles delay)
{
    event->xfer = this;
    event->startCycle = dtu.curCycle();

    if (event->isRead())
        bytesRead.sample(event->remaining);
    else
        bytesWritten.sample(event->remaining);

    dtu.schedule(event, dtu.clockEdge(Cycles(delay + 1)));

    // finish the noc request now to make the port unbusy
    if (event->isRemote())
        dtu.schedNocRequestFinished(dtu.clockEdge(Cycles(1)));
}

size_t
XferUnit::abortTransfers(AbortType type, int coreId, bool all)
{
    size_t count = 0;

    if (type != ABORT_ABORT)
    {
        for (size_t i = 0; i < bufCount; ++i)
        {
            auto ev = bufs[i]->event;
            if (!ev)
                continue;

            if (type == ABORT_LOCAL && !ev->isRemote())
            {
                ev->abort(Dtu::Error::ABORT);
                // by default, we auto-delete it, but in this case, we have to do
                // that manually since it's not the current event
                delete ev;
                count++;
            }
            else if (type == ABORT_REMOTE && ev->isRemote())
            {
                if (all || ev->senderCore() == coreId)
                {
                    ev->abort(Dtu::Error::ABORT);
                    delete ev;
                    count++;
                }
            }
        }

        // if we don't have any request of that core, remember the abort for
        // the future
        if (!all && count == 0)
        {
            DPRINTFS(DtuXfers, (&dtu),
                "Remembering transfer abort for PE%2d\n", coreId);
            abortReqs.push_back(coreId);
        }
    }
    else
    {
        auto it = std::find(abortReqs.begin(), abortReqs.end(), coreId);
        if (it != abortReqs.end())
        {
            DPRINTFS(DtuXfers, (&dtu),
                "Aborting abort for PE%2d\n", *it);
            abortReqs.erase(it);
            count++;
        }
    }

    return count;
}

void
XferUnit::recvMemResponse(uint64_t evId,
                          const void* data,
                          Addr size)
{
    Buffer *buf = getBuffer(evId);
    // ignore responses for aborted transfers
    if (!buf)
        return;

    assert(buf->event);

    if (data && buf->event->isRead())
    {
        assert(buf->offset + size <= bufSize);

        memcpy(buf->bytes + buf->offset, data, size);

        buf->offset += size;
    }

    // nothing more to copy?
    if (buf->event->remaining == 0)
    {
        buf->event->transferDone(buf->event->result);

        DPRINTFS(DtuXfers, (&dtu), "buf%d: Transfer done\n",
                 buf->id);

        // we're done with this buffer now
        if (buf->event->isRead())
            reads.sample(dtu.curCycle() - buf->event->startCycle);
        else
            writes.sample(dtu.curCycle() - buf->event->startCycle);
        buf->event->finish();
        buf->event = NULL;

        // start the next one, if there is any
        if (!queue.empty())
        {
            TransferEvent *ev = queue.front();
            queue.pop_front();
            dtu.schedule(ev, dtu.clockEdge(Cycles(1)));
        }
    }
    else
        buf->event->process();
}

XferUnit::Buffer *
XferUnit::getBuffer(uint64_t evId)
{
    for (size_t i = 0; i < bufCount; ++i)
    {
        if (bufs[i]->event && bufs[i]->event->id == evId)
            return bufs[i];
    }

    return NULL;
}

XferUnit::Buffer*
XferUnit::allocateBuf(TransferEvent *event, uint flags)
{
    // don't allow message receives in parallel. because otherwise we run into race conditions.
    // e.g., we could overwrite unread messages because we can't increase the message counter when
    // the receive starts (to not notify SW) and thus might start receiving without having space
    // another problem is that we might finish receiving the second message before the first and
    // then increase the message counter, so that the SW looks at the first message, which is not
    // ready yet.
    if (flags & XferFlags::MSGRECV)
    {
        for (size_t i = 0; i < bufCount; ++i)
        {
            if (bufs[i]->event && (bufs[i]->event->flags() & XferFlags::MSGRECV))
                return NULL;
        }
    }

    // the first buffer cannot cause pagefaults; thus we can only use it if for
    // transfers which abort if a pagefault is caused
    // this is required to resolve a deadlock due to additional transfers that
    // handle a already running pagefault transfer.
    size_t i = !dtu.tlb() || (flags & XferFlags::NOPF) ? 0 : 1;
    for (; i < bufCount; ++i)
    {
        if (!bufs[i]->event)
        {
            bufs[i]->event = event;
            bufs[i]->offset = 0;
            return bufs[i];
        }
    }

    return NULL;
}
