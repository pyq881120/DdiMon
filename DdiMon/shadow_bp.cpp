// Copyright (c) 2015-2016, tandasat. All rights reserved.
// Use of this source code is governed by a MIT-style license that can be
// found in the LICENSE file.

/// @file
/// Implements DdiMon core functions.

#include "shadow_bp.h"
#include "shadow_bp_internal.h"
#include <ntimage.h>
#define NTSTRSAFE_NO_CB_FUNCTIONS
#include <ntstrsafe.h>
#include "../HyperPlatform/HyperPlatform/common.h"
#include "../HyperPlatform/HyperPlatform/log.h"
#include "../HyperPlatform/HyperPlatform/util.h"
#include "../HyperPlatform/HyperPlatform/ept.h"

////////////////////////////////////////////////////////////////////////////////
//
// macro utilities
//

////////////////////////////////////////////////////////////////////////////////
//
// constants and macros
//

////////////////////////////////////////////////////////////////////////////////
//
// types
//

// Copy of a page seen by a guest as a result of memory shadowing
struct Page {
  UCHAR* page;  // A page aligned copy of a page
  Page();
  ~Page();
};

// Scoped lock
class ScopedSpinLockAtDpc {
 public:
  explicit ScopedSpinLockAtDpc(_In_ PKSPIN_LOCK spin_lock);

  ~ScopedSpinLockAtDpc();

 private:
  KLOCK_QUEUE_HANDLE lock_handle_;
};

////////////////////////////////////////////////////////////////////////////////
//
// prototypes
//

static std::unique_ptr<PatchInformation> SbppCreatePreBreakpoint(
    _In_ void* address, _In_ const BreakpointTarget& target,
    _In_ const char* name);

static std::unique_ptr<PatchInformation> SbppCreatePostBreakpoint(
    _In_ void* address, _In_ const PatchInformation& info,
    _In_ HANDLE target_tid, _In_ const CapturedParameters& parameters);

static std::unique_ptr<PatchInformation> SbppCreateBreakpoint(
    _In_ void* address);

static PatchInformation* SbppFindPatchInfoByAddress(_In_ void* address);

static PatchInformation* SbppFindPatchInfoByPage(_In_ void* address);

static PatchInformation* SbppFindDuplicatedPostPatchInfo(
    _In_ void* address, _In_ HANDLE target_tid);

static void SbppEmbedBreakpoint(_In_ void* address);

static void SbppEnablePageShadowingForExec(_In_ const PatchInformation& info,
                                           _In_ EptData* ept_data);

static void SbppEnablePageShadowingForRW(_In_ const PatchInformation& info,
                                         _In_ EptData* ept_data);

static void SbppDisablePageShadowing(_In_ const PatchInformation& info,
                                     _In_ EptData* ept_data);

static bool SbppIsShadowBreakpoint(_In_ const PatchInformation& info);

static void SbppSetMonitorTrapFlag(_In_ bool enable);

static void SbppSaveLastPatchInfo(_In_ const PatchInformation& info);

static const PatchInformation* SbppRestoreLastPatchInfo();

static bool SbppIsSbpActive();

static void SbppAddBreakpointToList(
    _In_ std::unique_ptr<PatchInformation> info);

static void SbppDeleteBreakpointFromList(_In_ const PatchInformation& info);

#if defined(ALLOC_PRAGMA)
#pragma alloc_text(INIT, SbpInitialization)
#pragma alloc_text(INIT, SbpStart)
#pragma alloc_text(PAGE, SbpTermination)
#endif

////////////////////////////////////////////////////////////////////////////////
//
// variables
//

// Holds all currently installed breakpoints
static std::vector<std::unique_ptr<PatchInformation>>* g_sbpp_breakpoints;

// Spin lock for g_sbpp_breakpoints
static KSPIN_LOCK g_sbpp_breakpoints_skinlock;

// Remember a breakpoint hit last
static const PatchInformation* g_sbpp_last_breakpoint;

// Remember a value of guests eflags.IT
static bool g_sbpp_previouse_guest_interrupt_flag;

////////////////////////////////////////////////////////////////////////////////
//
// implementations
//

// Initializes DdiMon
_Use_decl_annotations_ EXTERN_C NTSTATUS SbpInitialization() {
  KeInitializeSpinLock(&g_sbpp_breakpoints_skinlock);

  g_sbpp_breakpoints = new std::vector<std::unique_ptr<PatchInformation>>();

  return STATUS_SUCCESS;
}

_Use_decl_annotations_ EXTERN_C NTSTATUS SbpStart() {
  // Enables page shadowing for all breakpoints
  auto status = UtilVmCall(HypercallNumber::kDdimonEnablePageShadowing,
                           g_sbpp_breakpoints);
  return status;
}

// Enables page shadowing for all breakpoints
_Use_decl_annotations_ NTSTATUS SbpVmCallEnablePageShadowing(EptData* ept_data,
                                                             void* context) {
  HYPERPLATFORM_COMMON_DBG_BREAK();
  auto breakpoints =
      reinterpret_cast<std::vector<std::unique_ptr<PatchInformation>>*>(
          context);

  for (auto& info : *breakpoints) {
    SbppEnablePageShadowingForExec(*info, ept_data);
  }
  return STATUS_SUCCESS;
}

// Terminates DdiMon
_Use_decl_annotations_ EXTERN_C void SbpTermination() {
  PAGED_CODE();

  auto ptrs = g_sbpp_breakpoints;
  auto status = UtilVmCall(HypercallNumber::kDdimonDisablePageShadowing, ptrs);
  NT_VERIFY(NT_SUCCESS(status));
  UtilSleep(500);

  g_sbpp_breakpoints = nullptr;
  delete ptrs;
}

// Disables page shadowing for all breakpoints
_Use_decl_annotations_ void SbpVmCallDisablePageShadowing(EptData* ept_data,
                                                          void* context) {
  HYPERPLATFORM_COMMON_DBG_BREAK();
  const auto breakpoints =
      reinterpret_cast<std::vector<std::unique_ptr<PatchInformation>>*>(
          context);

  for (auto& info : *breakpoints) {
    SbppDisablePageShadowing(*info, ept_data);
  }
}

// Handles #BP. Determinas if the #BP is caused by a shadow breakpoint, and if
// so, runs its handler, switchs a page view to read/write shadow page and sets
// the monitor trap flag to execute only one instruction where is located on the
// read/write shadow page. Then saves the breakpoint info as the last event.
_Use_decl_annotations_ bool SbpHandleBreakpoint(EptData* ept_data,
                                                void* guest_ip,
                                                GpRegisters* gp_regs) {
  if (!SbppIsSbpActive()) {
    return false;
  }

  const auto info = SbppFindPatchInfoByAddress(guest_ip);
  if (!info) {
    return false;
  }

  if (!SbppIsShadowBreakpoint(*info)) {
    return false;
  }

  // DdiMon is unable to handle it
  if (KeGetCurrentIrql() > DISPATCH_LEVEL) {
    HYPERPLATFORM_COMMON_BUG_CHECK(HyperPlatformBugCheck::kUnspecified, 0, 0,
                                   0);
  }

  // VMM has to change the current CR3 to a guest's CR3 in order to access
  // memory address because VMM runs with System's CR3 saved in and restored
  // from
  // VmcsField::kHostCr3, while a guest's CR3 is depends on thread contexts.
  // Without using guest's CR3, it is likely that any use-address space is
  // inaccessible from a VMM ending up with a bug check.
  const auto guest_cr3 = UtilVmRead(VmcsField::kGuestCr3);
  const auto vmm_cr3 = __readcr3();

  if (info->type == BreakpointType::kPre) {
    // Pre breakpoint
    __writecr3(guest_cr3);
    info->handler(*info, ept_data, gp_regs, UtilVmRead(VmcsField::kGuestRsp));
    __writecr3(vmm_cr3);
    SbppEnablePageShadowingForRW(*info, ept_data);
    SbppSetMonitorTrapFlag(true);
    SbppSaveLastPatchInfo(*info);

  } else {
    // Post breakpoint
    if (info->target_tid == PsGetCurrentThreadId()) {
      // It is a target thread. Execute the post handler and let it continue
      // subsequence instructions.
      __writecr3(guest_cr3);
      info->handler(*info, ept_data, gp_regs, UtilVmRead(VmcsField::kGuestRsp));
      __writecr3(vmm_cr3);
      SbppDeleteBreakpointFromList(*info);
      // If there is another breakpoint on the same page, mamory shadowing for
      // the page cannot be deleted.
      if (!SbppFindPatchInfoByPage(guest_ip)) {
        SbppDisablePageShadowing(*info, ept_data);
      }
    } else {
      // It is not. Let it allow to run one instruction without breakpoint
      SbppEnablePageShadowingForRW(*info, ept_data);
      SbppSetMonitorTrapFlag(true);
      SbppSaveLastPatchInfo(*info);
    }
  }

  // Yes, it was caused by shadow breakpoint. Do not deliver the #BP to a guest.
  return true;
}

// Handles MTF VM-exit. Restores the last breakpoint event, re-enables stealth
// breakpoint and clears MTF;
_Use_decl_annotations_ void SbpHandleMonitorTrapFlag(EptData* ept_data) {
  NT_VERIFY(SbppIsSbpActive());

  const auto info = SbppRestoreLastPatchInfo();
  SbppEnablePageShadowingForExec(*info, ept_data);
  SbppSetMonitorTrapFlag(false);
}

// Handles EPT violation VM-exit.
_Use_decl_annotations_ void SbpHandleEptViolation(EptData* ept_data,
                                                  void* fault_va) {
  if (!SbppIsSbpActive()) {
    return;
  }
  const auto info = SbppFindPatchInfoByPage(fault_va);
  if (!info) {
    return;
  }

  // EPT violation was caused because a guest tried to read or write to a page
  // where currently set as execute only for protecting breakpoint. Let a guest
  // read or write a page from read/write shadow page and run a single
  // instruction.
  SbppEnablePageShadowingForRW(*info, ept_data);
  SbppSetMonitorTrapFlag(true);
  SbppSaveLastPatchInfo(*info);
}

// Creates Pre breakpoint object and adds it to the list
_Use_decl_annotations_ void SbpCreatePreBreakpoint(
    void* address, const BreakpointTarget& target, const char* name) {
  auto info =
      SbppCreatePreBreakpoint(reinterpret_cast<void*>(address), target, name);
  SbppAddBreakpointToList(std::move(info));
}

// Creats Post breakpoint object, adds it to the list and enables it
_Use_decl_annotations_ void SbpCreateAndEnablePostBreakpoint(
    void* address, const PatchInformation& info,
    const CapturedParameters& parameters, EptData* ept_data) {
  auto duplicated_info =
      SbppFindDuplicatedPostPatchInfo(address, PsGetCurrentThreadId());
  if (duplicated_info) {
    duplicated_info->parameters = parameters;
    return;
  }
  auto info_for_post = SbppCreatePostBreakpoint(
      address, info, PsGetCurrentThreadId(), parameters);
  auto ptr = info_for_post.get();
  SbppAddBreakpointToList(std::move(info_for_post));
  SbppEnablePageShadowingForExec(*ptr, ept_data);
}

// Creates Pre breakpoint object
_Use_decl_annotations_ static std::unique_ptr<PatchInformation>
SbppCreatePreBreakpoint(void* address, const BreakpointTarget& target,
                        const char* name) {
  auto info_for_pre = SbppCreateBreakpoint(address);
  info_for_pre->type = BreakpointType::kPre;
  info_for_pre->handler = target.pre_handler;
  info_for_pre->post_handler = target.post_handler;
  info_for_pre->target_tid = nullptr;
  info_for_pre->parameters = {};
  memcpy(info_for_pre->name.data(), name, info_for_pre->name.size() - 1);
  return info_for_pre;
}

// Creats Post breakpoint object
_Use_decl_annotations_ static std::unique_ptr<PatchInformation>
SbppCreatePostBreakpoint(void* address, const PatchInformation& info,
                         HANDLE target_tid,
                         const CapturedParameters& parameters) {
  auto info_for_post = SbppCreateBreakpoint(address);
  info_for_post->type = BreakpointType::kPost;
  info_for_post->handler = info.post_handler;
  info_for_post->post_handler = nullptr;
  info_for_post->target_tid = target_tid;
  info_for_post->parameters = parameters;
  info_for_post->name = info.name;
  return info_for_post;
}

// Creates a breakpoint object and fill out basic fields
_Use_decl_annotations_ static std::unique_ptr<PatchInformation>
SbppCreateBreakpoint(void* address) {
  auto info = std::make_unique<PatchInformation>();
  auto reusable_info = SbppFindPatchInfoByPage(address);
  if (reusable_info) {
    // Found an existing brekapoint object targetting the same page as this one.
    // re-use shadow pages.
    info->shadow_page_base_for_rw = reusable_info->shadow_page_base_for_rw;
    info->shadow_page_base_for_exec = reusable_info->shadow_page_base_for_exec;
  } else {
    // This breakpoint is for a page that is not currently set any breakpoint
    // (ie not shadowed). Creates shadow pages.
    info->shadow_page_base_for_rw = std::make_shared<Page>();
    info->shadow_page_base_for_exec = std::make_shared<Page>();
    auto page_base = PAGE_ALIGN(address);
    RtlCopyMemory(info->shadow_page_base_for_rw.get()->page, page_base,
                  PAGE_SIZE);
    RtlCopyMemory(info->shadow_page_base_for_exec.get()->page, page_base,
                  PAGE_SIZE);
  }
  info->patch_address = address;
  info->pa_base_for_rw =
      UtilPaFromVa(info->shadow_page_base_for_rw.get()->page);
  info->pa_base_for_exec =
      UtilPaFromVa(info->shadow_page_base_for_exec.get()->page);

  // Set an actual breakpoint (0xcc) onto the shadow page for EXEC
  SbppEmbedBreakpoint(info->shadow_page_base_for_exec.get()->page +
                      BYTE_OFFSET(address));
  return info;
}

// Find a breakpoint object by address
_Use_decl_annotations_ static PatchInformation* SbppFindPatchInfoByPage(
    void* address) {
  ScopedSpinLockAtDpc scoped_lock(&g_sbpp_breakpoints_skinlock);
  auto ptrs = g_sbpp_breakpoints;
  NT_ASSERT(ptrs);

  const auto found =
      std::find_if(ptrs->begin(), ptrs->end(), [address](const auto& info) {
        return PAGE_ALIGN(info->patch_address) == PAGE_ALIGN(address);
      });
  if (found == ptrs->cend()) {
    return nullptr;
  }
  return found->get();
}

// Find a breakpoint object that are on the same page as the address and its
// shadow pages are reusable
_Use_decl_annotations_ static PatchInformation* SbppFindPatchInfoByAddress(
    void* address) {
  ScopedSpinLockAtDpc scoped_lock(&g_sbpp_breakpoints_skinlock);
  auto ptrs = g_sbpp_breakpoints;
  NT_ASSERT(ptrs);

  auto found = std::find_if(
      ptrs->begin(), ptrs->end(),
      [address](const auto& info) { return info->patch_address == address; });
  if (found == ptrs->cend()) {
    return nullptr;
  }
  return found->get();
}

// Find a duplicated post breakpoint object. It is a workaround for the issue
// #2.
_Use_decl_annotations_ static PatchInformation* SbppFindDuplicatedPostPatchInfo(
    void* address, HANDLE target_tid) {
  ScopedSpinLockAtDpc scoped_lock(&g_sbpp_breakpoints_skinlock);
  auto ptrs = g_sbpp_breakpoints;
  NT_ASSERT(ptrs);

  const auto found = std::find_if(
      ptrs->begin(), ptrs->end(), [address, target_tid](const auto& info) {
        return (info->type == BreakpointType::kPost &&
                PAGE_ALIGN(info->patch_address) == PAGE_ALIGN(address) &&
                info->target_tid == target_tid);
      });
  if (found == ptrs->cend()) {
    return nullptr;
  }
  return found->get();
}

// Sets a breakpoint to the address
_Use_decl_annotations_ static void SbppEmbedBreakpoint(void* address) {
  static const UCHAR kBreakpoint[1] = {
      0xcc,
  };
  UtilForceCopyMemory(address, kBreakpoint, sizeof(kBreakpoint));
  KeInvalidateAllCaches();
}

// Show a shadowed page for execution
_Use_decl_annotations_ static void SbppEnablePageShadowingForExec(
    const PatchInformation& info, EptData* ept_data) {
  const auto ept_pt_entry =
      EptGetEptPtEntry(ept_data, UtilPaFromVa(info.patch_address));

  // Allow the VMM to redirect read and write access to the address by dening
  // those accesses and handling them on EPT violation
  ept_pt_entry->fields.write_access = false;
  ept_pt_entry->fields.read_access = false;

  // Only execute is allowed now to the adresss. Show the copied page for exec
  // that has an actual breakpoint to the guest.
  ept_pt_entry->fields.physial_address = UtilPfnFromPa(info.pa_base_for_exec);

  UtilInveptAll();
}

// Show a shadowed page for read and write
_Use_decl_annotations_ static void SbppEnablePageShadowingForRW(
    const PatchInformation& info, EptData* ept_data) {
  const auto ept_pt_entry =
      EptGetEptPtEntry(ept_data, UtilPaFromVa(info.patch_address));

  // Allow a guest to read and write as well as execute the address. Show the
  // copied page for read/write that does not have an breakpoint but reflects
  // all modification by a guest if that happened.
  ept_pt_entry->fields.write_access = true;
  ept_pt_entry->fields.read_access = true;
  ept_pt_entry->fields.physial_address = UtilPfnFromPa(info.pa_base_for_rw);

  UtilInveptAll();
}

// Stop showing a shadow page
_Use_decl_annotations_ static void SbppDisablePageShadowing(
    const PatchInformation& info, EptData* ept_data) {
  //    Replace with a fake copy
  const auto pa_base = UtilPaFromVa(PAGE_ALIGN(info.patch_address));
  const auto ept_pt_entry = EptGetEptPtEntry(ept_data, pa_base);
  ept_pt_entry->fields.execute_access = true;
  ept_pt_entry->fields.write_access = true;
  ept_pt_entry->fields.read_access = true;
  ept_pt_entry->fields.physial_address = UtilPfnFromPa(pa_base);
  UtilInveptAll();
}

// Checks if #BP is caused by the read write copy page. If so, that breakpoint
// is set by a guest and not the VMM and should be delivered to a guest.
_Use_decl_annotations_ static bool SbppIsShadowBreakpoint(
    const PatchInformation& info) {
  auto address = info.shadow_page_base_for_rw.get()->page +
                 BYTE_OFFSET(info.patch_address);
  return (*address != 0xcc);
}

// Set MTF on the current processor, and modifies guest's TF accordingly.
_Use_decl_annotations_ static void SbppSetMonitorTrapFlag(bool enable) {
  VmxProcessorBasedControls vm_procctl = {
      static_cast<unsigned int>(UtilVmRead(VmcsField::kCpuBasedVmExecControl))};
  vm_procctl.fields.monitor_trap_flag = enable;
  UtilVmWrite(VmcsField::kCpuBasedVmExecControl, vm_procctl.all);

  // When enabling MTF, disables maskable interrupt on a guest to ensure a next
  // execution occurs on the next instruction and not on an interrupt handler.
  // It is required because Windows can shedule clock interruption (eg, 0xd1) on
  // VM-enter when VMM took long time on VM-exit handling. The author is woking
  // hard for taking off this requirement. See #11 in the HyperPlatform project.
  FlagRegister flags = {UtilVmRead(VmcsField::kGuestRflags)};
  if (enable) {
    // clear IF
    g_sbpp_previouse_guest_interrupt_flag = flags.fields.intf;
    flags.fields.intf = false;
  } else {
    // restore IF
    flags.fields.intf = g_sbpp_previouse_guest_interrupt_flag;
  }
  UtilVmWrite(VmcsField::kGuestRflags, flags.all);
}

// Saves the breakpoint object as the last one.
_Use_decl_annotations_ static void SbppSaveLastPatchInfo(
    const PatchInformation& info) {
  NT_ASSERT(!g_sbpp_last_breakpoint);
  g_sbpp_last_breakpoint = &info;
}

// Retrieves the last info
/*_Use_decl_annotations_*/ static const PatchInformation*
SbppRestoreLastPatchInfo() {
  const auto info = g_sbpp_last_breakpoint;
  NT_ASSERT(info);
  g_sbpp_last_breakpoint = nullptr;
  return info;
}

// Checks if DdiMon is already initialized
/*_Use_decl_annotations_*/ static bool SbppIsSbpActive() {
  return !!(g_sbpp_breakpoints);
}

// Adds a breakpoint info to the list
_Use_decl_annotations_ static void SbppAddBreakpointToList(
    std::unique_ptr<PatchInformation> info) {
  ScopedSpinLockAtDpc scoped_lock(&g_sbpp_breakpoints_skinlock);
  NT_ASSERT(g_sbpp_breakpoints);
  g_sbpp_breakpoints->push_back(std::move(info));
}

// Deletes a breakpoint info from the list if exists
_Use_decl_annotations_ static void SbppDeleteBreakpointFromList(
    const PatchInformation& info) {
  ScopedSpinLockAtDpc scoped_lock(&g_sbpp_breakpoints_skinlock);
  auto ptrs = g_sbpp_breakpoints;
  NT_ASSERT(ptrs);
  auto iter =
      std::find_if(ptrs->begin(), ptrs->end(), [info](const auto& info2) {
        return (info.patch_address == info2->patch_address &&
                info.target_tid == info2->target_tid);
      });
  if (iter != ptrs->end()) {
    ptrs->erase(iter);
  }
}

// Allocates a non-paged, page-alined page. Issues bug check on failure
Page::Page()
    : page(reinterpret_cast<UCHAR*>(ExAllocatePoolWithTag(
          NonPagedPool, PAGE_SIZE, kHyperPlatformCommonPoolTag))) {
  if (!page) {
    HYPERPLATFORM_COMMON_BUG_CHECK(
        HyperPlatformBugCheck::kCritialPoolAllocationFailure, 0, 0, 0);
  }
}

// De-allocates the allocated page
Page::~Page() { ExFreePoolWithTag(page, kHyperPlatformCommonPoolTag); }

// Acquires a spin lock
ScopedSpinLockAtDpc::ScopedSpinLockAtDpc(_In_ PKSPIN_LOCK spin_lock) {
  KeAcquireInStackQueuedSpinLockAtDpcLevel(spin_lock, &lock_handle_);
}

// Releases a spin lock
ScopedSpinLockAtDpc::~ScopedSpinLockAtDpc() {
  KeReleaseInStackQueuedSpinLockFromDpcLevel(&lock_handle_);
}
