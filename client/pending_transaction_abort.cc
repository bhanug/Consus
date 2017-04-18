// Copyright (c) 2015-2016, Robert Escriva, Cornell University
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//     * Redistributions of source code must retain the above copyright notice,
//       this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above copyright
//       notice, this list of conditions and the following disclaimer in the
//       documentation and/or other materials provided with the distribution.
//     * Neither the name of Consus nor the names of its contributors may be
//       used to endorse or promote products derived from this software without
//       specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

// BusyBee
#include <busybee.h>

// consus
#include "common/constants.h"
#include "common/consus.h"
#include "client/client.h"
#include "client/pending_transaction_abort.h"
#include "client/transaction.h"

using consus::pending_transaction_abort;

pending_transaction_abort :: pending_transaction_abort(int64_t client_id,
                                                       consus_returncode* status,
                                                       transaction* xact,
                                                       uint64_t slot)
    : pending(client_id, status)
    , m_xact(xact)
    , m_ss()
    , m_slot(slot)
{
}

pending_transaction_abort :: ~pending_transaction_abort() throw ()
{
}

std::string
pending_transaction_abort :: describe()
{
    std::ostringstream ostr;
    ostr << "pending_transaction_abort(id=" << m_xact->txid() << "\")";
    return ostr.str();
}

void
pending_transaction_abort :: kickstart_state_machine(client* cl)
{
    m_xact->initialize(&m_ss);
    send_request(cl);
}

void
pending_transaction_abort :: handle_server_failure(client* cl, comm_id)
{
    send_request(cl);
}

void
pending_transaction_abort :: handle_server_disruption(client* cl, comm_id)
{
    send_request(cl);
}

void
pending_transaction_abort :: handle_busybee_op(client* cl,
                                               uint64_t,
                                               std::auto_ptr<e::buffer>,
                                               e::unpacker up)
{
    consus_returncode rc;
    up = up >> rc;

    if (up.error())
    {
        abort(); // XXX
    }

    this->success(); // XXX
    cl->add_to_returnable(this);
}

bool
pending_transaction_abort :: transaction_finished(client* cl, const transaction_group& tg, uint64_t outcome)
{
    if (reinterpret_cast<transaction*>(m_xact)->txid() != tg.txid)
    {
        return false;
    }

    if (outcome == CONSUS_VOTE_COMMIT)
    {
        PENDING_ERROR(INVALID) << "transaction requested abort, but ended up committing";
    }
    else if (outcome == CONSUS_VOTE_ABORT)
    {
        this->success();
    }
    else
    {
        PENDING_ERROR(SERVER_ERROR) << "transaction terminated in state unknown to the client";
    }

    cl->add_to_returnable(this);
    return true;
}

void
pending_transaction_abort :: send_request(client* cl)
{
    while (true)
    {
        const uint64_t nonce = m_xact->parent()->generate_new_nonce();
        const size_t sz = BUSYBEE_HEADER_SIZE
                        + pack_size(TXMAN_ABORT)
                        + pack_size(m_xact->txid())
                        + 2 * VARINT_64_MAX_SIZE;
        comm_id id = m_ss.next();

        if (id == comm_id())
        {
            m_xact->mark_aborted();
            PENDING_ERROR(UNAVAILABLE) << "insufficient number of servers to ensure durability";
            cl->add_to_returnable(this);
            return;
        }

        std::auto_ptr<e::buffer> msg(e::buffer::create(sz));
        msg->pack_at(BUSYBEE_HEADER_SIZE)
            << TXMAN_ABORT << m_xact->txid()
            << e::pack_varint(nonce)
            << e::pack_varint(m_slot);

        if (cl->send(nonce, id, msg, this))
        {
            return;
        }
    }
}
