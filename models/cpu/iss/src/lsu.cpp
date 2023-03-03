/*
 * Copyright (C) 2020 GreenWaves Technologies, SAS, ETH Zurich and
 *                    University of Bologna
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Authors: Germain Haugou, GreenWaves Technologies (germain.haugou@greenwaves-technologies.com)
 */

#include <vp/vp.hpp>
#include "iss.hpp"

void Lsu::reset(bool active)
{
    if (active)
    {
        this->elw_stalled.set(false);
        this->misaligned_size = 0;
    }
}

void Lsu::exec_misaligned(void *__this, vp::clock_event *event)
{
    Iss *iss = (Iss *)__this;
    Lsu *_this = &iss->lsu;

    _this->trace.msg(vp::trace::LEVEL_TRACE, "Handling second half of misaligned access\n");

    // As the 2 load accesses for misaligned access are generated by the
    // wrapper, we need to account the extra access here.
    // iss->timing.stall_misaligned_account();

    iss->timing.event_load_account(1);
    iss->timing.cycle_account();

    if (_this->data_req_aligned(_this->misaligned_addr, _this->misaligned_data,
                                _this->misaligned_size, _this->misaligned_is_write) == vp::IO_REQ_OK)
    {
        iss->trace.dump_trace_enabled = true;
        _this->stall_callback(_this);
        iss->exec.switch_to_full_mode();
    }
    else
    {
        _this->trace.warning("UNIMPLEMENTED AT %s %d\n", __FILE__, __LINE__);
    }
}


int Lsu::data_misaligned_req(iss_addr_t addr, uint8_t *data_ptr, int size, bool is_write)
{

    iss_addr_t addr0 = addr & ADDR_MASK;
    iss_addr_t addr1 = (addr + size - 1) & ADDR_MASK;

    this->trace.msg("Misaligned data request (addr: 0x%lx, size: 0x%x, is_write: %d)\n", addr, size, is_write);

    this->iss.timing.event_misaligned_account(1);

    // The access is a misaligned access
    // Change the event so that we can do the first access now and the next access
    // during the next cycle
    int size0 = addr1 - addr;
    int size1 = size - size0;

    // Remember the access properties for the second access
    this->misaligned_size = size1;
    this->misaligned_data = data_ptr + size0;
    this->misaligned_addr = addr1;
    this->misaligned_is_write = is_write;

    // And do the first one now
    int err = data_req_aligned(addr, data_ptr, size0, is_write);
    if (err == vp::IO_REQ_OK)
    {
        // As the transaction is split into 2 parts, we must tell the ISS
        // that the access is pending as the instruction must be executed
        // only when the second access is finished.
        this->iss.exec.instr_event->meth_set(&this->iss, &Lsu::exec_misaligned);
        this->iss.exec.insn_hold();
        return vp::IO_REQ_PENDING;
    }
    else
    {
        this->trace.force_warning("UNIMPLEMENTED AT %s %d, error during misaligned access\n", __FILE__, __LINE__);
        return vp::IO_REQ_INVALID;
    }
}

void Lsu::data_grant(void *__this, vp::io_req *req)
{
}

void Lsu::data_response(void *__this, vp::io_req *req)
{
    Lsu *_this = (Lsu *)__this;
    Iss *iss = &_this->iss;

    iss->exec.stalled_dec();

    _this->trace.msg("Received data response (stalled: %d)\n", iss->exec.stalled.get());

    // First call the ISS to finish the instruction
    _this->iss.timing.stall_load_account(req->get_latency());

    // Call the access termination callback only we the access is not misaligned since
    // in this case, the second access with handle it.
    if (_this->misaligned_size == 0)
    {
        _this->stall_callback(_this);
    }
}

int Lsu::data_req_aligned(iss_addr_t addr, uint8_t *data_ptr, int size, bool is_write)
{
    this->trace.msg("Data request (addr: 0x%lx, size: 0x%x, is_write: %d)\n", addr, size, is_write);
    vp::io_req *req = &this->io_req;
    req->init();
    req->set_addr(addr);
    req->set_size(size);
    req->set_is_write(is_write);
    req->set_data(data_ptr);
    int err = this->data.req(req);
    if (err == vp::IO_REQ_OK)
    {
        if (this->io_req.get_latency() > 0)
        {
            this->iss.timing.stall_load_account(req->get_latency());
        }
        return 0;
    }
    else if (err == vp::IO_REQ_INVALID)
    {
        vp_warning_always(&this->iss.top.warning,
                          "Invalid access (pc: 0x%" PRIxFULLREG ", offset: 0x%" PRIxFULLREG ", size: 0x%x, is_write: %d)\n",
                          this->iss.exec.current_insn->addr, addr, size, is_write);
        return err;
    }

    this->trace.msg(vp::trace::LEVEL_TRACE, "Waiting for asynchronous response\n");
    this->iss.exec.insn_stall();
    return err;
}

int Lsu::data_req(iss_addr_t addr, uint8_t *data_ptr, int size, bool is_write)
{
    iss_addr_t addr0 = addr & ADDR_MASK;
    iss_addr_t addr1 = (addr + size - 1) & ADDR_MASK;

    if (likely(addr0 == addr1))
        return this->data_req_aligned(addr, data_ptr, size, is_write);
    else
        return this->data_misaligned_req(addr, data_ptr, size, is_write);
}

Lsu::Lsu(Iss &iss)
    : iss(iss)
{
}

void Lsu::build()
{
    iss.top.traces.new_trace("lsu", &this->trace, vp::DEBUG);
    data.set_resp_meth(&Lsu::data_response);
    data.set_grant_meth(&Lsu::data_grant);
    this->iss.top.new_master_port(this, "data", &data);

    this->iss.top.new_reg("elw_stalled", &this->elw_stalled, false);

    this->io_req.set_data(new uint8_t[sizeof(iss_reg_t)]);
}

void Lsu::store_resume(Lsu *lsu)
{
    // For now we don't have to do anything as the register was written directly
    // by the request but we cold support sign-extended loads here;
    lsu->iss.exec.insn_terminate();
}

void Lsu::load_resume(Lsu *lsu)
{
    // Nothing to do, the zero-extension was done by initializing the register to 0
    lsu->iss.exec.insn_terminate();
}

void Lsu::elw_resume(Lsu *lsu)
{
    // Clear pending elw to not replay it when the next interrupt occurs
    lsu->iss.exec.insn_terminate();
    lsu->iss.exec.elw_insn = NULL;
    lsu->elw_stalled.set(false);
    lsu->iss.exec.busy_enter();
}

void Lsu::load_signed_resume(Lsu *lsu)
{
    lsu->iss.exec.insn_terminate();
    int reg = lsu->stall_reg;
    lsu->iss.regfile.set_reg(reg, iss_get_signed_value(lsu->iss.regfile.get_reg(reg),
        lsu->stall_size * 8));
}

void Lsu::load_boxed_resume(Lsu *lsu)
{
    lsu->iss.exec.insn_terminate();
    int reg = lsu->stall_reg;
    lsu->iss.regfile.set_reg(reg, iss_get_boxed_value(lsu->iss.regfile.get_reg(reg),
        lsu->stall_size * 8));
}

void Lsu::atomic(iss_insn_t *insn, iss_addr_t addr, int size, int reg_in, int reg_out,
    vp::io_req_opcode_e opcode)
{
        iss_addr_t phys_addr;

    this->trace.msg("Atomic request (addr: 0x%lx, size: 0x%x, opcode: %d)\n", addr, size, opcode);
    vp::io_req *req = &this->io_req;

    if (opcode == vp::io_req_opcode_e::LR)
    {
        if (this->iss.mmu.load_virt_to_phys(addr, phys_addr))
        {
            return;
        }
    }
    else
    {
        if (this->iss.mmu.store_virt_to_phys(addr, phys_addr))
        {
            return;
        }
    }

    req->init();
    req->set_addr(addr);
    req->set_size(size);
    req->set_opcode(opcode);
    req->set_data((uint8_t *)this->iss.regfile.reg_ref(reg_in));
    req->set_second_data((uint8_t *)this->iss.regfile.reg_ref(reg_out));
    req->set_initiator(this->iss.csr.mhartid);
    int err = this->data.req(req);
    if (err == vp::IO_REQ_OK)
    {
        if (size != ISS_REG_WIDTH/8)
        {
            this->iss.regfile.set_reg(reg_out, iss_get_signed_value(this->iss.regfile.get_reg(reg_out), size * 8));
        }

        if (this->io_req.get_latency() > 0)
        {
            this->iss.timing.stall_load_account(req->get_latency());
        }
        return;
    }
    else if (err == vp::IO_REQ_INVALID)
    {
        vp_warning_always(&this->iss.top.warning,
                          "Invalid atomic access (pc: 0x%" PRIxFULLREG ", offset: 0x%" PRIxFULLREG ", size: 0x%x, opcode: %d)\n",
                          this->iss.exec.current_insn->addr, addr, size, opcode);
        return;
    }

    this->trace.msg(vp::trace::LEVEL_TRACE, "Waiting for asynchronous response\n");
    this->iss.exec.insn_stall();

    if (size != ISS_REG_WIDTH/8)
    {
        this->stall_callback = &Lsu::load_signed_resume;
        this->stall_reg = reg_out;
        this->stall_size = size;
    }
    else
    {
        this->stall_callback = &Lsu::store_resume;
    }
}