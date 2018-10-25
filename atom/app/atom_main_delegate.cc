// Copyright (c) 2013 GitHub, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#include "atom/app/atom_main_delegate.h"

#include <iostream>
#include <memory>
#include <string>

#include "atom/app/atom_content_client.h"
#include "atom/browser/atom_browser_client.h"
#include "atom/browser/relauncher.h"
#include "atom/common/options_switches.h"
#include "atom/renderer/atom_renderer_client.h"
#include "atom/renderer/atom_sandboxed_renderer_client.h"
#include "atom/utility/atom_content_utility_client.h"
#include "base/command_line.h"
#include "base/debug/stack_trace.h"
#include "base/environment.h"
#include "base/logging.h"
#include "base/mac/bundle_locations.h"
#include "base/path_service.h"
#include "chrome/common/chrome_paths.h"
#include "content/public/common/content_switches.h"
#include "electron/buildflags/buildflags.h"
#include "ipc/ipc_buildflags.h"
#include "services/service_manager/embedder/switches.h"
#include "services/service_manager/sandbox/switches.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/resource/resource_bundle.h"
#include "ui/base/ui_base_switches.h"

#if BUILDFLAG(IPC_MESSAGE_LOG_ENABLED)
#define IPC_MESSAGE_MACROS_LOG_ENABLED
#include "content/public/common/content_ipc_logging.h"
#define IPC_LOG_TABLE_ADD_ENTRY(msg_id, logger) \
  content::RegisterIPCLogger(msg_id, logger)
#include "atom/common/common_message_generator.h"
#endif

#if defined(OS_MACOSX)
#include "atom/app/atom_main_delegate_mac.h"
#endif

namespace atom {

namespace {

const char* kRelauncherProcess = "relauncher";

bool IsBrowserProcess(base::CommandLine* cmd) {
  std::string process_type = cmd->GetSwitchValueASCII(::switches::kProcessType);
  return process_type.empty();
}

// Returns true if this subprocess type needs the ResourceBundle initialized
// and resources loaded.
bool SubprocessNeedsResourceBundle(const std::string& process_type) {
  return
#if defined(OS_POSIX) && !defined(OS_MACOSX)
      // The zygote process opens the resources for the renderers.
      process_type == service_manager::switches::kZygoteProcess ||
#endif
#if defined(OS_MACOSX)
      // Mac needs them too for scrollbar related images and for sandbox
      // profiles.
      process_type == ::switches::kPpapiPluginProcess ||
      process_type == ::switches::kPpapiBrokerProcess ||
      process_type == ::switches::kGpuProcess ||
#endif
      process_type == ::switches::kRendererProcess ||
      process_type == ::switches::kUtilityProcess;
}

#if defined(OS_WIN)
void InvalidParameterHandler(const wchar_t*,
                             const wchar_t*,
                             const wchar_t*,
                             unsigned int,
                             uintptr_t) {
  // noop.
}
#endif

}  // namespace

void LoadResourceBundle(const std::string& locale) {
  const bool initialized = ui::ResourceBundle::HasSharedInstance();
  if (initialized)
    ui::ResourceBundle::CleanupSharedInstance();

  // Load other resource files.
  base::FilePath pak_dir;
#if defined(OS_MACOSX)
  pak_dir =
      base::mac::FrameworkBundlePath().Append(FILE_PATH_LITERAL("Resources"));
#else
  base::PathService::Get(base::DIR_MODULE, &pak_dir);
#endif

  ui::ResourceBundle::InitSharedInstanceWithLocale(
      locale, nullptr, ui::ResourceBundle::LOAD_COMMON_RESOURCES);
  ui::ResourceBundle& bundle = ui::ResourceBundle::GetSharedInstance();
  bundle.ReloadLocaleResources(locale);
  bundle.AddDataPackFromPath(pak_dir.Append(FILE_PATH_LITERAL("resources.pak")),
                             ui::SCALE_FACTOR_NONE);
#if BUILDFLAG(ENABLE_PDF_VIEWER)
  NOTIMPLEMENTED()
      << "Hi, whoever's fixing PDF support! Thanks! The pdf "
         "viewer resources haven't been ported over to the GN build yet, so "
         "you'll probably need to change this bit of code.";
  bundle.AddDataPackFromPath(
      pak_dir.Append(FILE_PATH_LITERAL("pdf_viewer_resources.pak")),
      ui::GetSupportedScaleFactors()[0]);
#endif  // BUILDFLAG(ENABLE_PDF_VIEWER)
}

AtomMainDelegate::AtomMainDelegate() {}

AtomMainDelegate::~AtomMainDelegate() {}

bool AtomMainDelegate::BasicStartupComplete(int* exit_code) {
  auto* command_line = base::CommandLine::ForCurrentProcess();

  logging::LoggingSettings settings;
#if defined(OS_WIN)
  // On Windows the terminal returns immediately, so we add a new line to
  // prevent output in the same line as the prompt.
  if (IsBrowserProcess(command_line))
    std::wcout << std::endl;
#if defined(DEBUG)
  // Print logging to debug.log on Windows
  settings.logging_dest = logging::LOG_TO_ALL;
  settings.log_file = L"debug.log";
  settings.lock_log = logging::LOCK_LOG_FILE;
  settings.delete_old = logging::DELETE_OLD_LOG_FILE;
#else
  settings.logging_dest = logging::LOG_TO_SYSTEM_DEBUG_LOG;
#endif  // defined(DEBUG)
#else   // defined(OS_WIN)
  settings.logging_dest = logging::LOG_TO_SYSTEM_DEBUG_LOG;
#endif  // !defined(OS_WIN)

  // Only enable logging when --enable-logging is specified.
  auto env = base::Environment::Create();
  if (!command_line->HasSwitch(::switches::kEnableLogging) &&
      !env->HasVar("ELECTRON_ENABLE_LOGGING")) {
    settings.logging_dest = logging::LOG_NONE;
    logging::SetMinLogLevel(logging::LOG_NUM_SEVERITIES);
  }

  logging::InitLogging(settings);

  // Logging with pid and timestamp.
  logging::SetLogItems(true, false, true, false);

  // Enable convient stack printing.
#if defined(DEBUG) && defined(OS_LINUX)
  bool enable_stack_dumping = true;
#else
  bool enable_stack_dumping = env->HasVar("ELECTRON_ENABLE_STACK_DUMPING");
#endif
#if defined(ARCH_CPU_ARM_FAMILY) && defined(ARCH_CPU_32_BITS)
  // For 32bit ARM enabling stack printing would end up crashing.
  // https://github.com/electron/electron/pull/11230#issuecomment-363232482
  enable_stack_dumping = false;
#endif
  if (enable_stack_dumping)
    base::debug::EnableInProcessStackDumping();

  chrome::RegisterPathProvider();

#if defined(OS_MACOSX)
  OverrideChildProcessPath();
  OverrideFrameworkBundlePath();
  SetUpBundleOverrides();
#endif

#if defined(OS_WIN)
  // Ignore invalid parameter errors.
  _set_invalid_parameter_handler(InvalidParameterHandler);
  // Disable the ActiveVerifier, which is used by Chrome to track possible
  // bugs, but no use in Electron.
  base::win::DisableHandleVerifier();
#endif

  content_client_ = std::make_unique<AtomContentClient>();
  SetContentClient(content_client_.get());

  return false;
}

void AtomMainDelegate::PreSandboxStartup() {
  auto* command_line = base::CommandLine::ForCurrentProcess();

  std::string process_type =
      command_line->GetSwitchValueASCII(::switches::kProcessType);

  // Initialize ResourceBundle which handles files loaded from external
  // sources. The language should have been passed in to us from the
  // browser process as a command line flag.
  if (SubprocessNeedsResourceBundle(process_type)) {
    std::string locale = command_line->GetSwitchValueASCII(::switches::kLang);
    LoadResourceBundle(locale);
  }

  // Only append arguments for browser process.
  if (!IsBrowserProcess(command_line))
    return;

  if (!command_line->HasSwitch(switches::kEnableMixedSandbox)) {
    if (command_line->HasSwitch(switches::kEnableSandbox)) {
      // Disable setuid sandbox since it is not longer required on
      // linux(namespace sandbox is available on most distros).
      command_line->AppendSwitch(
          service_manager::switches::kDisableSetuidSandbox);
    } else {
      // Disable renderer sandbox for most of node's functions.
      command_line->AppendSwitch(service_manager::switches::kNoSandbox);
    }
  }

  // Allow file:// URIs to read other file:// URIs by default.
  command_line->AppendSwitch(::switches::kAllowFileAccessFromFiles);

#if defined(OS_MACOSX)
  // Enable AVFoundation.
  command_line->AppendSwitch("enable-avfoundation");
#endif
}

void AtomMainDelegate::PreCreateMainMessageLoop() {
#if defined(OS_MACOSX)
  RegisterAtomCrApp();
#endif
}

content::ContentBrowserClient* AtomMainDelegate::CreateContentBrowserClient() {
  browser_client_.reset(new AtomBrowserClient);
  return browser_client_.get();
}

content::ContentRendererClient*
AtomMainDelegate::CreateContentRendererClient() {
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kEnableSandbox) ||
      !base::CommandLine::ForCurrentProcess()->HasSwitch(
          service_manager::switches::kNoSandbox)) {
    renderer_client_.reset(new AtomSandboxedRendererClient);
  } else {
    renderer_client_.reset(new AtomRendererClient);
  }

  return renderer_client_.get();
}

content::ContentUtilityClient* AtomMainDelegate::CreateContentUtilityClient() {
  utility_client_.reset(new AtomContentUtilityClient);
  return utility_client_.get();
}

int AtomMainDelegate::RunProcess(
    const std::string& process_type,
    const content::MainFunctionParams& main_function_params) {
  if (process_type == kRelauncherProcess)
    return relauncher::RelauncherMain(main_function_params);
  else
    return -1;
}

#if defined(OS_MACOSX)
bool AtomMainDelegate::ShouldSendMachPort(const std::string& process_type) {
  return process_type != kRelauncherProcess;
}

bool AtomMainDelegate::DelaySandboxInitialization(
    const std::string& process_type) {
  return process_type == kRelauncherProcess;
}
#endif

}  // namespace atom
