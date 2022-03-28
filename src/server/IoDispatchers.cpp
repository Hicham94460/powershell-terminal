// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "precomp.h"

#include "IoDispatchers.h"

#include "ApiSorter.h"

#include "../host/conserv.h"
#include "../host/conwinuserrefs.h"
#include "../host/directio.h"
#include "../host/handle.h"
#include "../host/srvinit.h"
#include "../host/telemetry.hpp"

#include "../interactivity/base/HostSignalInputThread.hpp"
#include "../interactivity/inc/ServiceLocator.hpp"

#include "../types/inc/utils.hpp"

#include "IConsoleHandoff.h"

using namespace Microsoft::Console::Interactivity;
using namespace Microsoft::Console::Utils;

// From ntstatus.h, which we cannot include without causing a bunch of other conflicts. So we just include the one code we need.
//
// MessageId: STATUS_OBJECT_NAME_NOT_FOUND
//
// MessageText:
//
// Object Name not found.
//
#define STATUS_OBJECT_NAME_NOT_FOUND ((NTSTATUS)0xC0000034L)

// Routine Description:
// - This routine handles IO requests to create new objects. It validates the request, creates the object and a "handle" to it.
// Arguments:
// - pMessage - Supplies the message representing the create IO.
// Return Value:
// - A pointer to the reply message, if this message is to be completed inline; nullptr if this message will pend now and complete later.
PCONSOLE_API_MSG IoDispatchers::ConsoleCreateObject(_In_ PCONSOLE_API_MSG pMessage)
{
    NTSTATUS Status;

    CONSOLE_INFORMATION& gci = ServiceLocator::LocateGlobals().getConsoleInformation();
    PCD_CREATE_OBJECT_INFORMATION const CreateInformation = &pMessage->CreateObject;

    LockConsole();

    // If a generic object was requested, use the desired access to determine which type of object the caller is expecting.
    if (CreateInformation->ObjectType == CD_IO_OBJECT_TYPE_GENERIC)
    {
        if ((CreateInformation->DesiredAccess & (GENERIC_READ | GENERIC_WRITE)) == GENERIC_READ)
        {
            CreateInformation->ObjectType = CD_IO_OBJECT_TYPE_CURRENT_INPUT;
        }
        else if ((CreateInformation->DesiredAccess & (GENERIC_READ | GENERIC_WRITE)) == GENERIC_WRITE)
        {
            CreateInformation->ObjectType = CD_IO_OBJECT_TYPE_CURRENT_OUTPUT;
        }
    }

    std::unique_ptr<ConsoleHandleData> handle;
    // Check the requested type.
    switch (CreateInformation->ObjectType)
    {
    case CD_IO_OBJECT_TYPE_CURRENT_INPUT:
        Status = NTSTATUS_FROM_HRESULT(gci.pInputBuffer->AllocateIoHandle(ConsoleHandleData::HandleType::Input,
                                                                          CreateInformation->DesiredAccess,
                                                                          CreateInformation->ShareMode,
                                                                          handle));
        break;

    case CD_IO_OBJECT_TYPE_CURRENT_OUTPUT:
    {
        SCREEN_INFORMATION& ScreenInformation = gci.GetActiveOutputBuffer().GetMainBuffer();
        Status = NTSTATUS_FROM_HRESULT(ScreenInformation.AllocateIoHandle(ConsoleHandleData::HandleType::Output,
                                                                          CreateInformation->DesiredAccess,
                                                                          CreateInformation->ShareMode,
                                                                          handle));
        break;
    }
    case CD_IO_OBJECT_TYPE_NEW_OUTPUT:
        Status = ConsoleCreateScreenBuffer(handle, pMessage, CreateInformation, &pMessage->CreateScreenBuffer);
        break;

    default:
        Status = STATUS_INVALID_PARAMETER;
    }

    if (!NT_SUCCESS(Status))
    {
        UnlockConsole();
        pMessage->SetReplyStatus(Status);
        return pMessage;
    }

    auto deviceComm{ ServiceLocator::LocateGlobals().pDeviceComm };

    // Complete the request.
    pMessage->SetReplyStatus(STATUS_SUCCESS);
    pMessage->SetReplyInformation(deviceComm->PutHandle(handle.get()));

    if (SUCCEEDED(deviceComm->CompleteIo(&pMessage->Complete)))
    {
        // We've successfully transferred ownership of the handle to the driver. We can release and not free it.
        handle.release();
    }

    UnlockConsole();

    return nullptr;
}

// Routine Description:
// - This routine will handle a request to specifically close one of the console objects./
// Arguments:
// - pMessage - Supplies the message representing the close object IO.
// Return Value:
// - A pointer to the reply message.
PCONSOLE_API_MSG IoDispatchers::ConsoleCloseObject(_In_ PCONSOLE_API_MSG pMessage)
{
    LockConsole();

    delete pMessage->GetObjectHandle();
    pMessage->SetReplyStatus(STATUS_SUCCESS);

    UnlockConsole();
    return pMessage;
}

// LsaGetLoginSessionData might also fit the bill here, but it looks like it does RPC with lsass.exe. Using user32 is cheaper.
#pragma warning(suppress : 4505)
static bool _isInteractiveUserSession()
{
    DWORD sessionId{};
    if (ProcessIdToSessionId(GetCurrentProcessId(), &sessionId) && sessionId == 0)
    {
        return false;
    }

    // don't call CloseWindowStation on GetProcessWindowStation handle or switch this to a unique_hwinsta
    HWINSTA winsta{ GetProcessWindowStation() };
    if (winsta)
    {
        USEROBJECTFLAGS flags{};
        if (GetUserObjectInformationW(winsta, UOI_FLAGS, &flags, sizeof(flags), nullptr))
        {
            // An invisible window station suggests that we aren't interactive.
            return WI_IsFlagSet(flags.dwFlags, WSF_VISIBLE);
        }
    }

    // Assume that we are interactive if the flags can't be looked up or there's no window station
    return true;
}

// Routine Description:
// - Uses some information about current console state and
//   the incoming process state and preferences to determine
//   whether we should attempt to handoff to a registered console.
static bool _shouldAttemptHandoff(const Globals& globals,
                                  const CONSOLE_INFORMATION& gci,
                                  CONSOLE_API_CONNECTINFO& cac)
{
#if !TIL_FEATURE_ATTEMPTHANDOFF_ENABLED

    UNREFERENCED_PARAMETER(globals);
    UNREFERENCED_PARAMETER(gci);
    UNREFERENCED_PARAMETER(cac);

    // If we are outside of Windows, do not attempt a handoff
    // to another target as handoff is an inbox escape mechanism
    // to get to this copy!
    return false;

#else

    // Service desktops and non-interactive sessions should not
    // try to hand off -- they probably don't have any terminals
    // installed, and we don't want to risk breaking a service if
    // they *do*.
    if (!_isInteractiveUserSession())
    {
        return false;
    }

    // This console was started with a command line argument to
    // specifically block handoff to another console. We presume
    // this was for good reason (compatibility) and give up here.
    if (globals.launchArgs.GetForceNoHandoff())
    {
        return false;
    }

    // Someone double clicked this console or explicitly tried
    // to use it to launch a child process. Host it within this one
    // and do not hand off.
    if (globals.launchArgs.ShouldCreateServerHandle())
    {
        return false;
    }

    // This console is already initialized. Do not
    // attempt handoff to another one.
    // Note you can have a non-attach secondary connect for a child process
    // that is supposed to be inheriting the existing console/window from the parent.
    if (WI_IsFlagSet(gci.Flags, CONSOLE_INITIALIZED))
    {
        return false;
    }

    // If this is an AttachConsole message and not occurring
    // because of a conclnt!ConsoleInitialize, do not handoff.
    // ConsoleApp is FALSE for attach.
    if (!cac.ConsoleApp)
    {
        return false;
    }

    // If it is a PTY session, do not attempt handoff.
    if (globals.launchArgs.IsHeadless())
    {
        return false;
    }

    // If we do not have a registered handoff, do not attempt.
    if (!globals.handoffConsoleClsid)
    {
        return false;
    }

    // If we're already a target for receiving another handoff,
    // do not chain.
    if (globals.handoffTarget)
    {
        return false;
    }

    // If the client was started with CREATE_NO_WINDOW to CreateProcess,
    // this function will say that it does NOT deserve a visible window.
    // Return false.
    if (!ConsoleConnectionDeservesVisibleWindow(&cac))
    {
        return false;
    }

    // If the process is giving us explicit window show information, we need
    // to look at which one it is.
    if (WI_IsFlagSet(cac.ConsoleInfo.GetStartupFlags(), STARTF_USESHOWWINDOW))
    {
        switch (cac.ConsoleInfo.GetShowWindow())
        {
            // For all hide or minimize actions, do not hand off.
        case SW_HIDE:
        case SW_SHOWMINIMIZED:
        case SW_MINIMIZE:
        case SW_SHOWMINNOACTIVE:
        case SW_FORCEMINIMIZE:
            return false;
            // Intentionally fall through for all others
            // like maximize and show to hit the true below.
        }
    }

    return true;
#endif
}

// Routine Description:
// - Used when a client application establishes an initial connection to this console server.
// - This is supposed to represent accounting for the process, making the appropriate handles, etc.
// Arguments:
// - pReceiveMsg - The packet message received from the driver specifying that a client is connecting
// Return Value:
// - The response data to this request message.
PCONSOLE_API_MSG IoDispatchers::ConsoleHandleConnectionRequest(_In_ PCONSOLE_API_MSG pReceiveMsg)
{
    Globals& Globals = ServiceLocator::LocateGlobals();
    CONSOLE_INFORMATION& gci = Globals.getConsoleInformation();
    Telemetry::Instance().LogApiCall(Telemetry::ApiCall::AttachConsole);

    ConsoleProcessHandle* ProcessData = nullptr;
    NTSTATUS Status;

    LockConsole();

    const auto cleanup = wil::scope_exit([&]() noexcept {
        if (!NT_SUCCESS(Status))
        {
            pReceiveMsg->SetReplyStatus(Status);
            if (ProcessData != nullptr)
            {
                CommandHistory::s_Free(ProcessData);
                gci.ProcessHandleList.FreeProcessData(ProcessData);
            }
        }

        // FreeProcessData() above requires the console to be locked.
        UnlockConsole();
    });

    DWORD const dwProcessId = (DWORD)pReceiveMsg->Descriptor.Process;
    DWORD const dwThreadId = (DWORD)pReceiveMsg->Descriptor.Object;

    CONSOLE_API_CONNECTINFO Cac;
    Status = ConsoleInitializeConnectInfo(pReceiveMsg, &Cac);
    if (!NT_SUCCESS(Status))
    {
        return pReceiveMsg;
    }

    // If we pass the tests...
    // then attempt to delegate the startup to the registered replacement.
    if (_shouldAttemptHandoff(Globals, gci, Cac))
    {
        try
        {
            // Go get ourselves some COM.
            auto coinit = wil::CoInitializeEx(COINIT_MULTITHREADED);

            // Get the class/interface to the handoff handler. Local machine only.
            ::Microsoft::WRL::ComPtr<IConsoleHandoff> handoff;
            THROW_IF_FAILED(CoCreateInstance(Globals.handoffConsoleClsid.value(), nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&handoff)));

            // Pack up just enough of the attach message for the other console to process it.
            // NOTE: It can and will pick up the size/title/etc parameters from the driver again.
            CONSOLE_PORTABLE_ATTACH_MSG msg{};
            msg.IdHighPart = pReceiveMsg->Descriptor.Identifier.HighPart;
            msg.IdLowPart = pReceiveMsg->Descriptor.Identifier.LowPart;
            msg.Process = pReceiveMsg->Descriptor.Process;
            msg.Object = pReceiveMsg->Descriptor.Object;
            msg.Function = pReceiveMsg->Descriptor.Function;
            msg.InputSize = pReceiveMsg->Descriptor.InputSize;
            msg.OutputSize = pReceiveMsg->Descriptor.OutputSize;

            // Attempt to get server handle out of our own communication stack to pass it on.
            HANDLE serverHandle;
            THROW_IF_FAILED(Globals.pDeviceComm->GetServerHandle(&serverHandle));

            wil::unique_hfile signalPipeTheirSide;
            wil::unique_hfile signalPipeOurSide;

            THROW_IF_WIN32_BOOL_FALSE(CreatePipe(signalPipeOurSide.addressof(), signalPipeTheirSide.addressof(), nullptr, 0));

            // Give a copy of our own process handle to be tracked.
            wil::unique_process_handle ourProcess;
            THROW_IF_WIN32_BOOL_FALSE(DuplicateHandle(GetCurrentProcess(),
                                                      GetCurrentProcess(),
                                                      GetCurrentProcess(),
                                                      ourProcess.addressof(),
                                                      SYNCHRONIZE,
                                                      FALSE,
                                                      0));

            wil::unique_process_handle clientProcess;

            // Okay, moment of truth! If they say they successfully took it over, we're going to clean up.
            // If they fail, we'll throw here and it'll log and we'll just start normally.
            THROW_IF_FAILED(handoff->EstablishHandoff(serverHandle,
                                                      Globals.hInputEvent.get(),
                                                      &msg,
                                                      signalPipeTheirSide.get(),
                                                      ourProcess.get(),
                                                      &clientProcess));

            // Close handles for the things we gave to them
            signalPipeTheirSide.reset();
            ourProcess.reset();
            Globals.hInputEvent.reset();

            // Start a thread to listen for signals from their side that we must relay to the OS.
            auto hostSignalThread = std::make_unique<Microsoft::Console::HostSignalInputThread>(std::move(signalPipeOurSide));

            // Start it if it was successfully created.
            THROW_IF_FAILED(hostSignalThread->Start());

            TraceLoggingWrite(g_hConhostV2EventTraceProvider,
                              "ConsoleHandoffSucceeded",
                              TraceLoggingDescription("successfully handed off console connection"),
                              TraceLoggingGuid(Globals.handoffConsoleClsid.value(), "handoffCLSID"),
                              TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                              TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

            // Unlock in case anything tries to spool down as we exit.
            UnlockConsole();

            // We've handed off responsibility. Wait for child process to exit so we can maintain PID continuity for some clients.
            WaitForSingleObject(clientProcess.get(), INFINITE);

            // Exit process to clean up any outstanding things we have open.
            ExitProcess(S_OK);
        }
        catch (...)
        {
            const auto hr = wil::ResultFromCaughtException();

            TraceLoggingWrite(g_hConhostV2EventTraceProvider,
                              "ConsoleHandoffFailed",
                              TraceLoggingDescription("failed while attempting handoff"),
                              TraceLoggingGuid(Globals.handoffConsoleClsid.value(), "handoffCLSID"),
                              TraceLoggingHResult(hr),
                              TraceLoggingKeyword(MICROSOFT_KEYWORD_MEASURES),
                              TelemetryPrivacyDataTag(PDT_ProductAndServiceUsage));

            // Just log, don't do anything more. We'll move on to launching normally on failure.
            LOG_CAUGHT_EXCEPTION();
        }
    }

    Status = NTSTATUS_FROM_HRESULT(gci.ProcessHandleList.AllocProcessData(dwProcessId,
                                                                          dwThreadId,
                                                                          Cac.ProcessGroupId,
                                                                          nullptr,
                                                                          &ProcessData));

    if (!NT_SUCCESS(Status))
    {
        return pReceiveMsg;
    }

    ProcessData->fRootProcess = WI_IsFlagClear(gci.Flags, CONSOLE_INITIALIZED);

    // ConsoleApp will be false in the AttachConsole case.
    if (Cac.ConsoleApp)
    {
        LOG_IF_FAILED(ServiceLocator::LocateConsoleControl()->NotifyConsoleApplication(dwProcessId));
    }

    auto pNotifier = ServiceLocator::LocateAccessibilityNotifier();
    if (pNotifier)
    {
        pNotifier->NotifyConsoleStartApplicationEvent(dwProcessId);
    }

    if (WI_IsFlagClear(gci.Flags, CONSOLE_INITIALIZED))
    {
        Status = ConsoleAllocateConsole(&Cac);
        if (!NT_SUCCESS(Status))
        {
            return pReceiveMsg;
        }

        WI_SetFlag(gci.Flags, CONSOLE_INITIALIZED);
    }

    try
    {
        CommandHistory::s_Allocate({ Cac.AppName, Cac.AppNameLength / sizeof(wchar_t) }, (HANDLE)ProcessData);
    }
    catch (...)
    {
        LOG_CAUGHT_EXCEPTION();
        return pReceiveMsg;
    }

    const bool inConpty{ gci.IsInVtIoMode() };
    const bool hasFocus{ WI_IsFlagSet(gci.Flags, CONSOLE_HAS_FOCUS) };
    const auto grantFG{ inConpty || hasFocus };
    gci.ProcessHandleList.ModifyConsoleProcessFocus(grantFG);

    // Create the handles.

    Status = NTSTATUS_FROM_HRESULT(gci.pInputBuffer->AllocateIoHandle(ConsoleHandleData::HandleType::Input,
                                                                      GENERIC_READ | GENERIC_WRITE,
                                                                      FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                                      ProcessData->pInputHandle));

    if (!NT_SUCCESS(Status))
    {
        return pReceiveMsg;
    }

    auto& screenInfo = gci.GetActiveOutputBuffer().GetMainBuffer();
    Status = NTSTATUS_FROM_HRESULT(screenInfo.AllocateIoHandle(ConsoleHandleData::HandleType::Output,
                                                               GENERIC_READ | GENERIC_WRITE,
                                                               FILE_SHARE_READ | FILE_SHARE_WRITE,
                                                               ProcessData->pOutputHandle));

    if (!NT_SUCCESS(Status))
    {
        return pReceiveMsg;
    }

    // Complete the request.
    pReceiveMsg->SetReplyStatus(STATUS_SUCCESS);
    pReceiveMsg->SetReplyInformation(sizeof(CD_CONNECTION_INFORMATION));

    auto deviceComm{ ServiceLocator::LocateGlobals().pDeviceComm };
    CD_CONNECTION_INFORMATION ConnectionInformation = ProcessData->GetConnectionInformation(deviceComm);
    pReceiveMsg->Complete.Write.Data = &ConnectionInformation;
    pReceiveMsg->Complete.Write.Size = sizeof(CD_CONNECTION_INFORMATION);

    if (FAILED(deviceComm->CompleteIo(&pReceiveMsg->Complete)))
    {
        CommandHistory::s_Free((HANDLE)ProcessData);
        gci.ProcessHandleList.FreeProcessData(ProcessData);
    }

    Tracing::s_TraceConsoleAttachDetach(ProcessData, true);

    return nullptr;
}

// Routine Description:
// - This routine is called when a process is destroyed. It closes the process's handles and frees the console if it's the last reference.
// Arguments:
// - pProcessData - Pointer to the client's process information structure.
// Return Value:
// - A pointer to the reply message.
PCONSOLE_API_MSG IoDispatchers::ConsoleClientDisconnectRoutine(_In_ PCONSOLE_API_MSG pMessage)
{
    Telemetry::Instance().LogApiCall(Telemetry::ApiCall::FreeConsole);

    ConsoleProcessHandle* const pProcessData = pMessage->GetProcessHandle();

    auto pNotifier = ServiceLocator::LocateAccessibilityNotifier();
    if (pNotifier)
    {
        pNotifier->NotifyConsoleEndApplicationEvent(pProcessData->dwProcessId);
    }

    Tracing::s_TraceConsoleAttachDetach(pProcessData, false);

    LOG_IF_FAILED(RemoveConsole(pProcessData));

    pMessage->SetReplyStatus(STATUS_SUCCESS);

    return pMessage;
}

// Routine Description:
// - This routine validates a user IO and dispatches it to the appropriate worker routine.
// Arguments:
// - pMessage - Supplies the message representing the user IO.
// Return Value:
// - A pointer to the reply message, if this message is to be completed inline; nullptr if this message will pend now and complete later.
PCONSOLE_API_MSG IoDispatchers::ConsoleDispatchRequest(_In_ PCONSOLE_API_MSG pMessage)
{
    return ApiSorter::ConsoleDispatchRequest(pMessage);
}
