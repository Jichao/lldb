//===-- ThreadPlanGotoUser.cpp ---------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

// C Includes
// C++ Includes
// Other libraries and framework includes
// Project includes
#include "lldb/Target/ThreadPlanGotoUser.h"
#include "lldb/Core/Log.h"
#include "lldb/Core/Stream.h"
#include "lldb/core/Module.h"
#include "lldb/core/Section.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/RegisterContext.h"
#include "lldb/Target/StopInfo.h"
#include "lldb/Target/Target.h"

using namespace lldb;
using namespace lldb_private;
//----------------------------------------------------------------------
// ThreadPlanGotoUser: Step over the current instruction
//----------------------------------------------------------------------

ThreadPlanGotoUser::ThreadPlanGotoUser(Thread &thread,
                                                     bool step_over,
                                                     bool stop_other_threads,
                                                     Vote stop_vote,
                                                     Vote run_vote)
: ThreadPlan(ThreadPlan::eKindGotoUser,
             "step util user land", thread, stop_vote, run_vote),
m_instruction_addr(0), m_stop_other_threads(stop_other_threads),
m_step_over(step_over) {
    m_start_time = time(NULL);
    m_takes_iteration_count = true;
    SetUpState();
    m_start_address = 0;
    m_end_address = 0;
    m_done = false;
}

ThreadPlanGotoUser::~ThreadPlanGotoUser() = default;

void ThreadPlanGotoUser::SetUpState() {
    m_instruction_addr = m_thread.GetRegisterContext()->GetPC(0);
    StackFrameSP start_frame_sp(m_thread.GetStackFrameAtIndex(0));
    m_stack_id = start_frame_sp->GetStackID();
    
    m_start_has_symbol =
    start_frame_sp->GetSymbolContext(eSymbolContextSymbol).symbol != nullptr;
    
    StackFrameSP parent_frame_sp = m_thread.GetStackFrameAtIndex(1);
    if (parent_frame_sp)
        m_parent_frame_id = parent_frame_sp->GetStackID();
}

void ThreadPlanGotoUser::GetDescription(Stream *s,
                                               lldb::DescriptionLevel level) {
    if (level == lldb::eDescriptionLevelBrief) {
        if (m_step_over)
            s->Printf("instruction step over");
        else
            s->Printf("instruction step into");
    } else {
        s->Printf("Stepping one instruction past ");
        s->Address(m_instruction_addr, sizeof(addr_t));
        if (!m_start_has_symbol)
            s->Printf(" which has no symbol");
        
        if (m_step_over)
            s->Printf(" stepping over calls");
        else
            s->Printf(" stepping into calls");
    }
}

bool ThreadPlanGotoUser::ValidatePlan(Stream *error) {
    // Since we read the instruction we're stepping over from the thread,
    // this plan will always work.
    return true;
}

bool ThreadPlanGotoUser::DoPlanExplainsStop(Event *event_ptr) {
    StopInfoSP stop_info_sp = GetPrivateStopInfo();
    if (stop_info_sp) {
        StopReason reason = stop_info_sp->GetStopReason();
        return (reason == eStopReasonTrace || reason == eStopReasonNone);
    }
    return false;
}

bool ThreadPlanGotoUser::IsPlanStale() {
    Log *log(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_STEP));
    StackID cur_frame_id = m_thread.GetStackFrameAtIndex(0)->GetStackID();
    if (cur_frame_id == m_stack_id) {
        return (m_thread.GetRegisterContext()->GetPC(0) != m_instruction_addr);
    } else if (cur_frame_id < m_stack_id) {
        // If the current frame is younger than the start frame and we are stepping
        // over, then we need to continue,
        // but if we are doing just one step, we're done.
        return !m_step_over;
    } else {
        if (log) {
            log->Printf("ThreadPlanGotoUser::IsPlanStale - Current frame is "
                        "older than start frame, plan is stale.");
        }
        return true;
    }
}

lldb::addr_t ThreadPlanGotoUser::GetModuleBaseAddr(size_t index) {
    lldb::addr_t addr;
    Target& target = m_thread.GetProcess()->GetTarget();
    SectionList *section_list =
            target.GetImages().GetModuleAtIndex(index)->GetSectionList();
    addr = UINT64_MAX;
    for (size_t i = 0; i < section_list->GetSize(); ++i) {
        addr = std::min(addr, section_list->GetSectionAtIndex(i)->GetLoadBaseAddress(&target));
    }
    return addr;
}

bool ThreadPlanGotoUser::GetMainModuleAddr() {
    if (m_start_address || m_end_address)
        return true;

    m_start_address = GetModuleBaseAddr(0);
    m_end_address = GetModuleBaseAddr(1);
    printf("start address : 0x%" PRIx64  " end address: 0x%" PRIx64 "\n", m_start_address, m_end_address);
    return true;
}

bool ThreadPlanGotoUser::ShouldStop(Event *event_ptr) {
    lldb::addr_t pc_addr = m_thread.GetRegisterContext()->GetPC(0);
    GetMainModuleAddr();
    if (pc_addr >= m_start_address && pc_addr <= m_end_address) {
        if (!m_done) {
            printf("goto user time consumed : %li s\n", time(NULL) - m_start_time);
            m_done = true;
        }
        SetPlanComplete();
        return true;
    }
    return false;

//    lldb::addr_t pc_addr = m_thread.GetRegisterContext()->GetPC(0);
//    std::unique_ptr<lldb_private::Address> addr(new lldb_private::Address());
//    Target& target = m_thread.GetProcess()->GetTarget();
//    addr->SetLoadAddress(pc_addr, &target);
//
//    if (addr->GetModule() == target.GetImages().GetModuleAtIndex(0)) {
//        printf("goto user time consumed : %li s\n", time(NULL) - m_start_time);
//        SetPlanComplete();
//        return true;
//    }
    return false;
}

bool ThreadPlanGotoUser::StopOthers() { return m_stop_other_threads; }

StateType ThreadPlanGotoUser::GetPlanRunState() {
    return eStateStepping;
}

bool ThreadPlanGotoUser::WillStop() { return true; }

bool ThreadPlanGotoUser::MischiefManaged() {
    if (IsPlanComplete()) {
        Log *log(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_STEP));
        if (log)
            log->Printf("Completed single instruction step plan.");
        ThreadPlan::MischiefManaged();
        return true;
    } else {
        return false;
    }
}
