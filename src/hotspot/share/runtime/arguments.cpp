/*
 * Copyright (c) 1997, 2018, Oracle and/or its affiliates. All rights reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

#include "precompiled.hpp"
#include "jvm.h"
#include "classfile/classLoader.hpp"
#include "classfile/javaAssertions.hpp"
#include "classfile/moduleEntry.hpp"
#include "classfile/stringTable.hpp"
#include "classfile/symbolTable.hpp"
#include "gc/shared/collectorPolicy.hpp"
#include "gc/shared/gcArguments.hpp"
#include "gc/shared/gcConfig.hpp"
#include "logging/log.hpp"
#include "logging/logConfiguration.hpp"
#include "logging/logStream.hpp"
#include "logging/logTag.hpp"
#include "memory/allocation.inline.hpp"
#include "memory/universe.hpp"
#include "oops/oop.inline.hpp"
#include "prims/jvmtiExport.hpp"
#include "runtime/arguments.hpp"
#include "runtime/arguments_ext.hpp"
#include "runtime/flags/jvmFlag.hpp"
#include "runtime/flags/jvmFlagConstraintList.hpp"
#include "runtime/flags/jvmFlagWriteableList.hpp"
#include "runtime/flags/jvmFlagRangeList.hpp"
#include "runtime/globals_extension.hpp"
#include "runtime/java.hpp"
#include "runtime/os.inline.hpp"
#include "runtime/safepoint.hpp"
#include "runtime/safepointMechanism.hpp"
#include "runtime/vm_version.hpp"
#include "services/management.hpp"
#include "services/memTracker.hpp"
#include "utilities/align.hpp"
#include "utilities/defaultStream.hpp"
#include "utilities/macros.hpp"
#include "utilities/stringUtils.hpp"
#if INCLUDE_JVMCI
#include "jvmci/jvmciRuntime.hpp"
#endif
#if INCLUDE_JFR
#include "jfr/jfr.hpp"
#endif

#define DEFAULT_JAVA_LAUNCHER  "generic"

char*  Arguments::_jvm_flags_file               = NULL;
char** Arguments::_jvm_flags_array              = NULL;
int    Arguments::_num_jvm_flags                = 0;
char** Arguments::_jvm_args_array               = NULL;
int    Arguments::_num_jvm_args                 = 0;
char*  Arguments::_java_command                 = NULL;
SystemProperty* Arguments::_system_properties   = NULL;
const char*  Arguments::_gc_log_filename        = NULL;
size_t Arguments::_conservative_max_heap_alignment = 0;
size_t Arguments::_min_heap_size                = 0;
Arguments::Mode Arguments::_mode                = _mixed;
bool   Arguments::_java_compiler                = false;
bool   Arguments::_xdebug_mode                  = false;
const char*  Arguments::_java_vendor_url_bug    = NULL;
const char*  Arguments::_sun_java_launcher      = DEFAULT_JAVA_LAUNCHER;
int    Arguments::_sun_java_launcher_pid        = -1;
bool   Arguments::_sun_java_launcher_is_altjvm  = false;

// These parameters are reset in method parse_vm_init_args()
bool   Arguments::_AlwaysCompileLoopMethods     = AlwaysCompileLoopMethods;
bool   Arguments::_UseOnStackReplacement        = UseOnStackReplacement;
bool   Arguments::_BackgroundCompilation        = BackgroundCompilation;
bool   Arguments::_ClipInlining                 = ClipInlining;
intx   Arguments::_Tier3InvokeNotifyFreqLog     = Tier3InvokeNotifyFreqLog;
intx   Arguments::_Tier4InvocationThreshold     = Tier4InvocationThreshold;

bool   Arguments::_enable_preview               = false;

char*  Arguments::SharedArchivePath             = NULL;

AgentLibraryList Arguments::_libraryList;
AgentLibraryList Arguments::_agentList;

abort_hook_t     Arguments::_abort_hook         = NULL;
exit_hook_t      Arguments::_exit_hook          = NULL;
vfprintf_hook_t  Arguments::_vfprintf_hook      = NULL;


SystemProperty *Arguments::_sun_boot_library_path = NULL;
SystemProperty *Arguments::_java_library_path = NULL;
SystemProperty *Arguments::_java_home = NULL;
SystemProperty *Arguments::_java_class_path = NULL;
SystemProperty *Arguments::_jdk_boot_class_path_append = NULL;
SystemProperty *Arguments::_vm_info = NULL;

GrowableArray<ModulePatchPath*> *Arguments::_patch_mod_prefix = NULL;
PathString *Arguments::_system_boot_class_path = NULL;
bool Arguments::_has_jimage = false;

char* Arguments::_ext_dirs = NULL;

bool PathString::set_value(const char *value) {
  if (_value != NULL) {
    FreeHeap(_value);
  }
  _value = AllocateHeap(strlen(value)+1, mtArguments);
  assert(_value != NULL, "Unable to allocate space for new path value");
  if (_value != NULL) {
    strcpy(_value, value);
  } else {
    // not able to allocate
    return false;
  }
  return true;
}

void PathString::append_value(const char *value) {
  char *sp;
  size_t len = 0;
  if (value != NULL) {
    len = strlen(value);
    if (_value != NULL) {
      len += strlen(_value);
    }
    sp = AllocateHeap(len+2, mtArguments);
    assert(sp != NULL, "Unable to allocate space for new append path value");
    if (sp != NULL) {
      if (_value != NULL) {
        strcpy(sp, _value);
        strcat(sp, os::path_separator());
        strcat(sp, value);
        FreeHeap(_value);
      } else {
        strcpy(sp, value);
      }
      _value = sp;
    }
  }
}

PathString::PathString(const char* value) {
  if (value == NULL) {
    _value = NULL;
  } else {
    _value = AllocateHeap(strlen(value)+1, mtArguments);
    strcpy(_value, value);
  }
}

PathString::~PathString() {
  if (_value != NULL) {
    FreeHeap(_value);
    _value = NULL;
  }
}

ModulePatchPath::ModulePatchPath(const char* module_name, const char* path) {
  assert(module_name != NULL && path != NULL, "Invalid module name or path value");
  size_t len = strlen(module_name) + 1;
  _module_name = AllocateHeap(len, mtInternal);
  strncpy(_module_name, module_name, len); // copy the trailing null
  _path =  new PathString(path);
}

ModulePatchPath::~ModulePatchPath() {
  if (_module_name != NULL) {
    FreeHeap(_module_name);
    _module_name = NULL;
  }
  if (_path != NULL) {
    delete _path;
    _path = NULL;
  }
}

SystemProperty::SystemProperty(const char* key, const char* value, bool writeable, bool internal) : PathString(value) {
  if (key == NULL) {
    _key = NULL;
  } else {
    _key = AllocateHeap(strlen(key)+1, mtArguments);
    strcpy(_key, key);
  }
  _next = NULL;
  _internal = internal;
  _writeable = writeable;
}

AgentLibrary::AgentLibrary(const char* name, const char* options,
               bool is_absolute_path, void* os_lib,
               bool instrument_lib) {
  _name = AllocateHeap(strlen(name)+1, mtArguments);
  strcpy(_name, name);
  if (options == NULL) {
    _options = NULL;
  } else {
    _options = AllocateHeap(strlen(options)+1, mtArguments);
    strcpy(_options, options);
  }
  _is_absolute_path = is_absolute_path;
  _os_lib = os_lib;
  _next = NULL;
  _state = agent_invalid;
  _is_static_lib = false;
  _is_instrument_lib = instrument_lib;
}

// Check if head of 'option' matches 'name', and sets 'tail' to the remaining
// part of the option string.
static bool match_option(const JavaVMOption *option, const char* name,
                         const char** tail) {
  size_t len = strlen(name);
  if (strncmp(option->optionString, name, len) == 0) {
    *tail = option->optionString + len;
    return true;
  } else {
    return false;
  }
}

// Check if 'option' matches 'name'. No "tail" is allowed.
static bool match_option(const JavaVMOption *option, const char* name) {
  const char* tail = NULL;
  bool result = match_option(option, name, &tail);
  if (tail != NULL && *tail == '\0') {
    return result;
  } else {
    return false;
  }
}

// Return true if any of the strings in null-terminated array 'names' matches.
// If tail_allowed is true, then the tail must begin with a colon; otherwise,
// the option must match exactly.
static bool match_option(const JavaVMOption* option, const char** names, const char** tail,
  bool tail_allowed) {
  for (/* empty */; *names != NULL; ++names) {
  if (match_option(option, *names, tail)) {
      if (**tail == '\0' || (tail_allowed && **tail == ':')) {
        return true;
      }
    }
  }
  return false;
}

#if INCLUDE_JFR
// return true on failure
static bool match_jfr_option(const JavaVMOption** option) {
  assert((*option)->optionString != NULL, "invariant");
  char* tail = NULL;
  if (match_option(*option, "-XX:StartFlightRecording", (const char**)&tail)) {
    return Jfr::on_start_flight_recording_option(option, tail);
  } else if (match_option(*option, "-XX:FlightRecorderOptions", (const char**)&tail)) {
    return Jfr::on_flight_recorder_option(option, tail);
  }
  return false;
}
#endif

static void logOption(const char* opt) {
  if (PrintVMOptions) {
    jio_fprintf(defaultStream::output_stream(), "VM option '%s'\n", opt);
  }
}

bool needs_module_property_warning = false;

#define MODULE_PROPERTY_PREFIX "jdk.module."
#define MODULE_PROPERTY_PREFIX_LEN 11
#define ADDEXPORTS "addexports"
#define ADDEXPORTS_LEN 10
#define ADDREADS "addreads"
#define ADDREADS_LEN 8
#define ADDOPENS "addopens"
#define ADDOPENS_LEN 8
#define PATCH "patch"
#define PATCH_LEN 5
#define ADDMODS "addmods"
#define ADDMODS_LEN 7
#define LIMITMODS "limitmods"
#define LIMITMODS_LEN 9
#define PATH "path"
#define PATH_LEN 4
#define UPGRADE_PATH "upgrade.path"
#define UPGRADE_PATH_LEN 12

void Arguments::add_init_library(const char* name, char* options) {
  _libraryList.add(new AgentLibrary(name, options, false, NULL));
}

void Arguments::add_init_agent(const char* name, char* options, bool absolute_path) {
  _agentList.add(new AgentLibrary(name, options, absolute_path, NULL));
}

void Arguments::add_instrument_agent(const char* name, char* options, bool absolute_path) {
  _agentList.add(new AgentLibrary(name, options, absolute_path, NULL, true));
}

// Late-binding agents not started via arguments
void Arguments::add_loaded_agent(AgentLibrary *agentLib) {
  _agentList.add(agentLib);
}

void Arguments::add_loaded_agent(const char* name, char* options, bool absolute_path, void* os_lib) {
  _agentList.add(new AgentLibrary(name, options, absolute_path, os_lib));
}

// Return TRUE if option matches 'property', or 'property=', or 'property.'.
static bool matches_property_suffix(const char* option, const char* property, size_t len) {
  return ((strncmp(option, property, len) == 0) &&
          (option[len] == '=' || option[len] == '.' || option[len] == '\0'));
}

// Return true if property starts with "jdk.module." and its ensuing chars match
// any of the reserved module properties.
// property should be passed without the leading "-D".
bool Arguments::is_internal_module_property(const char* property) {
  assert((strncmp(property, "-D", 2) != 0), "Unexpected leading -D");
  if  (strncmp(property, MODULE_PROPERTY_PREFIX, MODULE_PROPERTY_PREFIX_LEN) == 0) {
    const char* property_suffix = property + MODULE_PROPERTY_PREFIX_LEN;
    if (matches_property_suffix(property_suffix, ADDEXPORTS, ADDEXPORTS_LEN) ||
        matches_property_suffix(property_suffix, ADDREADS, ADDREADS_LEN) ||
        matches_property_suffix(property_suffix, ADDOPENS, ADDOPENS_LEN) ||
        matches_property_suffix(property_suffix, PATCH, PATCH_LEN) ||
        matches_property_suffix(property_suffix, ADDMODS, ADDMODS_LEN) ||
        matches_property_suffix(property_suffix, LIMITMODS, LIMITMODS_LEN) ||
        matches_property_suffix(property_suffix, PATH, PATH_LEN) ||
        matches_property_suffix(property_suffix, UPGRADE_PATH, UPGRADE_PATH_LEN)) {
      return true;
    }
  }
  return false;
}

// Process java launcher properties.
void Arguments::process_sun_java_launcher_properties(JavaVMInitArgs* args) {
  // See if sun.java.launcher, sun.java.launcher.is_altjvm or
  // sun.java.launcher.pid is defined.
  // Must do this before setting up other system properties,
  // as some of them may depend on launcher type.
  for (int index = 0; index < args->nOptions; index++) {
    const JavaVMOption* option = args->options + index;
    const char* tail;

    if (match_option(option, "-Dsun.java.launcher=", &tail)) {
      process_java_launcher_argument(tail, option->extraInfo);
      continue;
    }
    if (match_option(option, "-Dsun.java.launcher.is_altjvm=", &tail)) {
      if (strcmp(tail, "true") == 0) {
        _sun_java_launcher_is_altjvm = true;
      }
      continue;
    }
    if (match_option(option, "-Dsun.java.launcher.pid=", &tail)) {
      _sun_java_launcher_pid = atoi(tail);
      continue;
    }
  }
}

// Initialize system properties key and value.
void Arguments::init_system_properties() {

  // Set up _system_boot_class_path which is not a property but
  // relies heavily on argument processing and the jdk.boot.class.path.append
  // property. It is used to store the underlying system boot class path.
  _system_boot_class_path = new PathString(NULL);

  PropertyList_add(&_system_properties, new SystemProperty("java.vm.specification.name",
                                                           "Java Virtual Machine Specification",  false));
  PropertyList_add(&_system_properties, new SystemProperty("java.vm.version", VM_Version::vm_release(),  false));
  PropertyList_add(&_system_properties, new SystemProperty("java.vm.name", VM_Version::vm_name(),  false));
  PropertyList_add(&_system_properties, new SystemProperty("jdk.debug", VM_Version::jdk_debug_level(),  false));

  // Initialize the vm.info now, but it will need updating after argument parsing.
  _vm_info = new SystemProperty("java.vm.info", VM_Version::vm_info_string(), true);

  // Following are JVMTI agent writable properties.
  // Properties values are set to NULL and they are
  // os specific they are initialized in os::init_system_properties_values().
  _sun_boot_library_path = new SystemProperty("sun.boot.library.path", NULL,  true);
  _java_library_path = new SystemProperty("java.library.path", NULL,  true);
  _java_home =  new SystemProperty("java.home", NULL,  true);
  _java_class_path = new SystemProperty("java.class.path", "",  true);
  // jdk.boot.class.path.append is a non-writeable, internal property.
  // It can only be set by either:
  //    - -Xbootclasspath/a:
  //    - AddToBootstrapClassLoaderSearch during JVMTI OnLoad phase
  _jdk_boot_class_path_append = new SystemProperty("jdk.boot.class.path.append", "", false, true);

  // Add to System Property list.
  PropertyList_add(&_system_properties, _sun_boot_library_path);
  PropertyList_add(&_system_properties, _java_library_path);
  PropertyList_add(&_system_properties, _java_home);
  PropertyList_add(&_system_properties, _java_class_path);
  PropertyList_add(&_system_properties, _jdk_boot_class_path_append);
  PropertyList_add(&_system_properties, _vm_info);

  // Set OS specific system properties values
  os::init_system_properties_values();
}

// Update/Initialize System properties after JDK version number is known
void Arguments::init_version_specific_system_properties() {
  enum { bufsz = 16 };
  char buffer[bufsz];
  const char* spec_vendor = "Oracle Corporation";
  uint32_t spec_version = JDK_Version::current().major_version();

  jio_snprintf(buffer, bufsz, UINT32_FORMAT, spec_version);

  PropertyList_add(&_system_properties,
      new SystemProperty("java.vm.specification.vendor",  spec_vendor, false));
  PropertyList_add(&_system_properties,
      new SystemProperty("java.vm.specification.version", buffer, false));
  PropertyList_add(&_system_properties,
      new SystemProperty("java.vm.vendor", VM_Version::vm_vendor(),  false));
}

/*
 *  -XX argument processing:
 *
 *  -XX arguments are defined in several places, such as:
 *      globals.hpp, globals_<cpu>.hpp, globals_<os>.hpp, <compiler>_globals.hpp, or <gc>_globals.hpp.
 *  -XX arguments are parsed in parse_argument().
 *  -XX argument bounds checking is done in check_vm_args_consistency().
 *
 * Over time -XX arguments may change. There are mechanisms to handle common cases:
 *
 *      ALIASED: An option that is simply another name for another option. This is often
 *               part of the process of deprecating a flag, but not all aliases need
 *               to be deprecated.
 *
 *               Create an alias for an option by adding the old and new option names to the
 *               "aliased_jvm_flags" table. Delete the old variable from globals.hpp (etc).
 *
 *   DEPRECATED: An option that is supported, but a warning is printed to let the user know that
 *               support may be removed in the future. Both regular and aliased options may be
 *               deprecated.
 *
 *               Add a deprecation warning for an option (or alias) by adding an entry in the
 *               "special_jvm_flags" table and setting the "deprecated_in" field.
 *               Often an option "deprecated" in one major release will
 *               be made "obsolete" in the next. In this case the entry should also have its
 *               "obsolete_in" field set.
 *
 *     OBSOLETE: An option that has been removed (and deleted from globals.hpp), but is still accepted
 *               on the command line. A warning is printed to let the user know that option might not
 *               be accepted in the future.
 *
 *               Add an obsolete warning for an option by adding an entry in the "special_jvm_flags"
 *               table and setting the "obsolete_in" field.
 *
 *      EXPIRED: A deprecated or obsolete option that has an "accept_until" version less than or equal
 *               to the current JDK version. The system will flatly refuse to admit the existence of
 *               the flag. This allows a flag to die automatically over JDK releases.
 *
 *               Note that manual cleanup of expired options should be done at major JDK version upgrades:
 *                  - Newly expired options should be removed from the special_jvm_flags and aliased_jvm_flags tables.
 *                  - Newly obsolete or expired deprecated options should have their global variable
 *                    definitions removed (from globals.hpp, etc) and related implementations removed.
 *
 * Recommended approach for removing options:
 *
 * To remove options commonly used by customers (e.g. product -XX options), use
 * the 3-step model adding major release numbers to the deprecate, obsolete and expire columns.
 *
 * To remove internal options (e.g. diagnostic, experimental, develop options), use
 * a 2-step model adding major release numbers to the obsolete and expire columns.
 *
 * To change the name of an option, use the alias table as well as a 2-step
 * model adding major release numbers to the deprecate and expire columns.
 * Think twice about aliasing commonly used customer options.
 *
 * There are times when it is appropriate to leave a future release number as undefined.
 *
 * Tests:  Aliases should be tested in VMAliasOptions.java.
 *         Deprecated options should be tested in VMDeprecatedOptions.java.
 */

// The special_jvm_flags table declares options that are being deprecated and/or obsoleted. The
// "deprecated_in" or "obsolete_in" fields may be set to "undefined", but not both.
// When the JDK version reaches 'deprecated_in' limit, the JVM will process this flag on
// the command-line as usual, but will issue a warning.
// When the JDK version reaches 'obsolete_in' limit, the JVM will continue accepting this flag on
// the command-line, while issuing a warning and ignoring the flag value.
// Once the JDK version reaches 'expired_in' limit, the JVM will flatly refuse to admit the
// existence of the flag.
//
// MANUAL CLEANUP ON JDK VERSION UPDATES:
// This table ensures that the handling of options will update automatically when the JDK
// version is incremented, but the source code needs to be cleanup up manually:
// - As "deprecated" options age into "obsolete" or "expired" options, the associated "globals"
//   variable should be removed, as well as users of the variable.
// - As "deprecated" options age into "obsolete" options, move the entry into the
//   "Obsolete Flags" section of the table.
// - All expired options should be removed from the table.
static SpecialFlag const special_jvm_flags[] = {
  // -------------- Deprecated Flags --------------
  // --- Non-alias flags - sorted by obsolete_in then expired_in:
  { "MaxGCMinorPauseMillis",        JDK_Version::jdk(8), JDK_Version::undefined(), JDK_Version::undefined() },
  { "UseConcMarkSweepGC",           JDK_Version::jdk(9), JDK_Version::undefined(), JDK_Version::undefined() },
  { "AssumeMP",                     JDK_Version::jdk(10),JDK_Version::undefined(), JDK_Version::undefined() },
  { "MonitorInUseLists",            JDK_Version::jdk(10),JDK_Version::undefined(), JDK_Version::undefined() },
  { "MaxRAMFraction",               JDK_Version::jdk(10),  JDK_Version::undefined(), JDK_Version::undefined() },
  { "MinRAMFraction",               JDK_Version::jdk(10),  JDK_Version::undefined(), JDK_Version::undefined() },
  { "InitialRAMFraction",           JDK_Version::jdk(10),  JDK_Version::undefined(), JDK_Version::undefined() },
  { "UseMembar",                    JDK_Version::jdk(10), JDK_Version::undefined(), JDK_Version::undefined() },
  { "IgnoreUnverifiableClassesDuringDump", JDK_Version::jdk(10),  JDK_Version::undefined(), JDK_Version::undefined() },
  { "CompilerThreadHintNoPreempt",  JDK_Version::jdk(11), JDK_Version::jdk(12), JDK_Version::jdk(13) },
  { "VMThreadHintNoPreempt",        JDK_Version::jdk(11), JDK_Version::jdk(12), JDK_Version::jdk(13) },
  { "PrintSafepointStatistics",     JDK_Version::jdk(11), JDK_Version::jdk(12), JDK_Version::jdk(13) },
  { "PrintSafepointStatisticsTimeout", JDK_Version::jdk(11), JDK_Version::jdk(12), JDK_Version::jdk(13) },
  { "PrintSafepointStatisticsCount",JDK_Version::jdk(11), JDK_Version::jdk(12), JDK_Version::jdk(13) },
  { "AggressiveOpts",               JDK_Version::jdk(11), JDK_Version::jdk(12), JDK_Version::jdk(13) },
  { "AllowNonVirtualCalls",         JDK_Version::jdk(11), JDK_Version::jdk(12), JDK_Version::jdk(13) },
  { "UnlinkSymbolsALot",            JDK_Version::jdk(11), JDK_Version::jdk(12), JDK_Version::jdk(13) },

  // --- Deprecated alias flags (see also aliased_jvm_flags) - sorted by obsolete_in then expired_in:
  { "DefaultMaxRAMFraction",        JDK_Version::jdk(8),  JDK_Version::undefined(), JDK_Version::undefined() },
  { "CreateMinidumpOnCrash",        JDK_Version::jdk(9),  JDK_Version::undefined(), JDK_Version::undefined() },
  { "MustCallLoadClassInternal",    JDK_Version::jdk(10), JDK_Version::jdk(11), JDK_Version::jdk(12) },
  { "UnsyncloadClass",              JDK_Version::jdk(10), JDK_Version::jdk(11), JDK_Version::jdk(12) },

  // -------------- Obsolete Flags - sorted by expired_in --------------
  { "CheckAssertionStatusDirectives",JDK_Version::undefined(), JDK_Version::jdk(11), JDK_Version::jdk(12) },
  { "PrintMallocFree",               JDK_Version::undefined(), JDK_Version::jdk(11), JDK_Version::jdk(12) },
  { "PrintMalloc",                   JDK_Version::undefined(), JDK_Version::jdk(11), JDK_Version::jdk(12) },
  { "ShowSafepointMsgs",             JDK_Version::undefined(), JDK_Version::jdk(11), JDK_Version::jdk(12) },
  { "FastTLABRefill",                JDK_Version::jdk(10),     JDK_Version::jdk(11), JDK_Version::jdk(12) },
  { "SafepointSpinBeforeYield",      JDK_Version::jdk(10),     JDK_Version::jdk(11), JDK_Version::jdk(12) },
  { "CheckEndorsedAndExtDirs",       JDK_Version::jdk(10),     JDK_Version::jdk(11), JDK_Version::jdk(12) },
  { "DeferThrSuspendLoopCount",      JDK_Version::jdk(10),     JDK_Version::jdk(11), JDK_Version::jdk(12) },
  { "DeferPollingPageLoopCount",     JDK_Version::jdk(10),     JDK_Version::jdk(11), JDK_Version::jdk(12) },
  { "TraceScavenge",                 JDK_Version::undefined(), JDK_Version::jdk(11), JDK_Version::jdk(12) },
  { "PermSize",                      JDK_Version::undefined(), JDK_Version::jdk(8),  JDK_Version::undefined() },
  { "MaxPermSize",                   JDK_Version::undefined(), JDK_Version::jdk(8),  JDK_Version::undefined() },
  { "SharedReadWriteSize",           JDK_Version::undefined(), JDK_Version::jdk(10), JDK_Version::undefined() },
  { "SharedReadOnlySize",            JDK_Version::undefined(), JDK_Version::jdk(10), JDK_Version::undefined() },
  { "SharedMiscDataSize",            JDK_Version::undefined(), JDK_Version::jdk(10), JDK_Version::undefined() },
  { "SharedMiscCodeSize",            JDK_Version::undefined(), JDK_Version::jdk(10), JDK_Version::undefined() },
  { "UseUTCFileTimestamp",           JDK_Version::undefined(), JDK_Version::jdk(11), JDK_Version::jdk(12) },
  { "UseAppCDS",                     JDK_Version::undefined(), JDK_Version::jdk(11), JDK_Version::jdk(12) },
  { "InlineNotify",                  JDK_Version::undefined(), JDK_Version::jdk(11), JDK_Version::jdk(12) },
  { "EnableTracing",                 JDK_Version::undefined(), JDK_Version::jdk(11), JDK_Version::jdk(12) },
  { "UseLockedTracing",              JDK_Version::undefined(), JDK_Version::jdk(11), JDK_Version::jdk(12) },
  { "NativeMonitorTimeout",          JDK_Version::undefined(), JDK_Version::jdk(11), JDK_Version::jdk(12) },
  { "NativeMonitorSpinLimit",        JDK_Version::undefined(), JDK_Version::jdk(11), JDK_Version::jdk(12) },
  { "NativeMonitorFlags",            JDK_Version::undefined(), JDK_Version::jdk(11), JDK_Version::jdk(12) },

#ifdef TEST_VERIFY_SPECIAL_JVM_FLAGS
  { "dep > obs",                    JDK_Version::jdk(9), JDK_Version::jdk(8), JDK_Version::undefined() },
  { "dep > exp ",                   JDK_Version::jdk(9), JDK_Version::undefined(), JDK_Version::jdk(8) },
  { "obs > exp ",                   JDK_Version::undefined(), JDK_Version::jdk(9), JDK_Version::jdk(8) },
  { "not deprecated or obsolete",   JDK_Version::undefined(), JDK_Version::undefined(), JDK_Version::jdk(9) },
  { "dup option",                   JDK_Version::jdk(9), JDK_Version::undefined(), JDK_Version::undefined() },
  { "dup option",                   JDK_Version::jdk(9), JDK_Version::undefined(), JDK_Version::undefined() },
  { "BytecodeVerificationRemote",   JDK_Version::undefined(), JDK_Version::jdk(9), JDK_Version::undefined() },
#endif

  { NULL, JDK_Version(0), JDK_Version(0) }
};

// Flags that are aliases for other flags.
typedef struct {
  const char* alias_name;
  const char* real_name;
} AliasedFlag;

static AliasedFlag const aliased_jvm_flags[] = {
  { "DefaultMaxRAMFraction",    "MaxRAMFraction"    },
  { "CreateMinidumpOnCrash",    "CreateCoredumpOnCrash" },
  { NULL, NULL}
};

// NOTE: A compatibility request will be necessary for each alias to be removed.
static AliasedLoggingFlag const aliased_logging_flags[] = {
  { "PrintCompressedOopsMode",   LogLevel::Info,  true,  LOG_TAGS(gc, heap, coops) },
  { "PrintSharedSpaces",         LogLevel::Info,  true,  LOG_TAGS(cds) },
  { "TraceBiasedLocking",        LogLevel::Info,  true,  LOG_TAGS(biasedlocking) },
  { "TraceClassLoading",         LogLevel::Info,  true,  LOG_TAGS(class, load) },
  { "TraceClassLoadingPreorder", LogLevel::Debug, true,  LOG_TAGS(class, preorder) },
  { "TraceClassPaths",           LogLevel::Info,  true,  LOG_TAGS(class, path) },
  { "TraceClassResolution",      LogLevel::Debug, true,  LOG_TAGS(class, resolve) },
  { "TraceClassUnloading",       LogLevel::Info,  true,  LOG_TAGS(class, unload) },
  { "TraceExceptions",           LogLevel::Info,  true,  LOG_TAGS(exceptions) },
  { "TraceLoaderConstraints",    LogLevel::Info,  true,  LOG_TAGS(class, loader, constraints) },
  { "TraceMonitorInflation",     LogLevel::Debug, true,  LOG_TAGS(monitorinflation) },
  { "TraceSafepointCleanupTime", LogLevel::Info,  true,  LOG_TAGS(safepoint, cleanup) },
  { "TraceJVMTIObjectTagging",   LogLevel::Debug, true,  LOG_TAGS(jvmti, objecttagging) },
  { "TraceRedefineClasses",      LogLevel::Info,  false, LOG_TAGS(redefine, class) },
  { NULL,                        LogLevel::Off,   false, LOG_TAGS(_NO_TAG) }
};

#ifndef PRODUCT
// These options are removed in jdk9. Remove this code for jdk10.
static AliasedFlag const removed_develop_logging_flags[] = {
  { "TraceClassInitialization",   "-Xlog:class+init" },
  { "TraceClassLoaderData",       "-Xlog:class+loader+data" },
  { "TraceDefaultMethods",        "-Xlog:defaultmethods=debug" },
  { "TraceItables",               "-Xlog:itables=debug" },
  { "TraceMonitorMismatch",       "-Xlog:monitormismatch=info" },
  { "TraceSafepoint",             "-Xlog:safepoint=debug" },
  { "TraceStartupTime",           "-Xlog:startuptime" },
  { "TraceVMOperation",           "-Xlog:vmoperation=debug" },
  { "PrintVtables",               "-Xlog:vtables=debug" },
  { "VerboseVerification",        "-Xlog:verification" },
  { NULL, NULL }
};
#endif //PRODUCT

// Return true if "v" is less than "other", where "other" may be "undefined".
static bool version_less_than(JDK_Version v, JDK_Version other) {
  assert(!v.is_undefined(), "must be defined");
  if (!other.is_undefined() && v.compare(other) >= 0) {
    return false;
  } else {
    return true;
  }
}

extern bool lookup_special_flag_ext(const char *flag_name, SpecialFlag& flag);

static bool lookup_special_flag(const char *flag_name, SpecialFlag& flag) {
  // Allow extensions to have priority
  if (lookup_special_flag_ext(flag_name, flag)) {
    return true;
  }

  for (size_t i = 0; special_jvm_flags[i].name != NULL; i++) {
    if ((strcmp(special_jvm_flags[i].name, flag_name) == 0)) {
      flag = special_jvm_flags[i];
      return true;
    }
  }
  return false;
}

bool Arguments::is_obsolete_flag(const char *flag_name, JDK_Version* version) {
  assert(version != NULL, "Must provide a version buffer");
  SpecialFlag flag;
  if (lookup_special_flag(flag_name, flag)) {
    if (!flag.obsolete_in.is_undefined()) {
      if (!version_less_than(JDK_Version::current(), flag.obsolete_in)) {
        *version = flag.obsolete_in;
        return true;
      }
    }
  }
  return false;
}

int Arguments::is_deprecated_flag(const char *flag_name, JDK_Version* version) {
  assert(version != NULL, "Must provide a version buffer");
  SpecialFlag flag;
  if (lookup_special_flag(flag_name, flag)) {
    if (!flag.deprecated_in.is_undefined()) {
      if (version_less_than(JDK_Version::current(), flag.obsolete_in) &&
          version_less_than(JDK_Version::current(), flag.expired_in)) {
        *version = flag.deprecated_in;
        return 1;
      } else {
        return -1;
      }
    }
  }
  return 0;
}

#ifndef PRODUCT
const char* Arguments::removed_develop_logging_flag_name(const char* name){
  for (size_t i = 0; removed_develop_logging_flags[i].alias_name != NULL; i++) {
    const AliasedFlag& flag = removed_develop_logging_flags[i];
    if (strcmp(flag.alias_name, name) == 0) {
      return flag.real_name;
    }
  }
  return NULL;
}
#endif // PRODUCT

const char* Arguments::real_flag_name(const char *flag_name) {
  for (size_t i = 0; aliased_jvm_flags[i].alias_name != NULL; i++) {
    const AliasedFlag& flag_status = aliased_jvm_flags[i];
    if (strcmp(flag_status.alias_name, flag_name) == 0) {
        return flag_status.real_name;
    }
  }
  return flag_name;
}

#ifdef ASSERT
static bool lookup_special_flag(const char *flag_name, size_t skip_index) {
  for (size_t i = 0; special_jvm_flags[i].name != NULL; i++) {
    if ((i != skip_index) && (strcmp(special_jvm_flags[i].name, flag_name) == 0)) {
      return true;
    }
  }
  return false;
}

// Verifies the correctness of the entries in the special_jvm_flags table.
// If there is a semantic error (i.e. a bug in the table) such as the obsoletion
// version being earlier than the deprecation version, then a warning is issued
// and verification fails - by returning false. If it is detected that the table
// is out of date, with respect to the current version, then a warning is issued
// but verification does not fail. This allows the VM to operate when the version
// is first updated, without needing to update all the impacted flags at the
// same time.
static bool verify_special_jvm_flags() {
  bool success = true;
  for (size_t i = 0; special_jvm_flags[i].name != NULL; i++) {
    const SpecialFlag& flag = special_jvm_flags[i];
    if (lookup_special_flag(flag.name, i)) {
      warning("Duplicate special flag declaration \"%s\"", flag.name);
      success = false;
    }
    if (flag.deprecated_in.is_undefined() &&
        flag.obsolete_in.is_undefined()) {
      warning("Special flag entry \"%s\" must declare version deprecated and/or obsoleted in.", flag.name);
      success = false;
    }

    if (!flag.deprecated_in.is_undefined()) {
      if (!version_less_than(flag.deprecated_in, flag.obsolete_in)) {
        warning("Special flag entry \"%s\" must be deprecated before obsoleted.", flag.name);
        success = false;
      }

      if (!version_less_than(flag.deprecated_in, flag.expired_in)) {
        warning("Special flag entry \"%s\" must be deprecated before expired.", flag.name);
        success = false;
      }
    }

    if (!flag.obsolete_in.is_undefined()) {
      if (!version_less_than(flag.obsolete_in, flag.expired_in)) {
        warning("Special flag entry \"%s\" must be obsoleted before expired.", flag.name);
        success = false;
      }

      // if flag has become obsolete it should not have a "globals" flag defined anymore.
      if (!version_less_than(JDK_Version::current(), flag.obsolete_in)) {
        if (JVMFlag::find_flag(flag.name) != NULL) {
          // Temporarily disable the warning: 8196739
          // warning("Global variable for obsolete special flag entry \"%s\" should be removed", flag.name);
        }
      }
    }

    if (!flag.expired_in.is_undefined()) {
      // if flag has become expired it should not have a "globals" flag defined anymore.
      if (!version_less_than(JDK_Version::current(), flag.expired_in)) {
        if (JVMFlag::find_flag(flag.name) != NULL) {
          // Temporarily disable the warning: 8196739
          // warning("Global variable for expired flag entry \"%s\" should be removed", flag.name);
        }
      }
    }

  }
  return success;
}
#endif

// Parses a size specification string.
bool Arguments::atojulong(const char *s, julong* result) {
  julong n = 0;

  // First char must be a digit. Don't allow negative numbers or leading spaces.
  if (!isdigit(*s)) {
    return false;
  }

  bool is_hex = (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'));
  char* remainder;
  errno = 0;
  n = strtoull(s, &remainder, (is_hex ? 16 : 10));
  if (errno != 0) {
    return false;
  }

  // Fail if no number was read at all or if the remainder contains more than a single non-digit character.
  if (remainder == s || strlen(remainder) > 1) {
    return false;
  }

  switch (*remainder) {
    case 'T': case 't':
      *result = n * G * K;
      // Check for overflow.
      if (*result/((julong)G * K) != n) return false;
      return true;
    case 'G': case 'g':
      *result = n * G;
      if (*result/G != n) return false;
      return true;
    case 'M': case 'm':
      *result = n * M;
      if (*result/M != n) return false;
      return true;
    case 'K': case 'k':
      *result = n * K;
      if (*result/K != n) return false;
      return true;
    case '\0':
      *result = n;
      return true;
    default:
      return false;
  }
}

Arguments::ArgsRange Arguments::check_memory_size(julong size, julong min_size, julong max_size) {
  if (size < min_size) return arg_too_small;
  if (size > max_size) return arg_too_big;
  return arg_in_range;
}

// Describe an argument out of range error
void Arguments::describe_range_error(ArgsRange errcode) {
  switch(errcode) {
  case arg_too_big:
    jio_fprintf(defaultStream::error_stream(),
                "The specified size exceeds the maximum "
                "representable size.\n");
    break;
  case arg_too_small:
  case arg_unreadable:
  case arg_in_range:
    // do nothing for now
    break;
  default:
    ShouldNotReachHere();
  }
}

static bool set_bool_flag(const char* name, bool value, JVMFlag::Flags origin) {
  if (JVMFlag::boolAtPut(name, &value, origin) == JVMFlag::SUCCESS) {
    return true;
  } else {
    return false;
  }
}

static bool set_fp_numeric_flag(const char* name, char* value, JVMFlag::Flags origin) {
  char* end;
  errno = 0;
  double v = strtod(value, &end);
  if ((errno != 0) || (*end != 0)) {
    return false;
  }

  if (JVMFlag::doubleAtPut(name, &v, origin) == JVMFlag::SUCCESS) {
    return true;
  }
  return false;
}

static bool set_numeric_flag(const char* name, char* value, JVMFlag::Flags origin) {
  julong v;
  int int_v;
  intx intx_v;
  bool is_neg = false;
  JVMFlag* result = JVMFlag::find_flag(name, strlen(name));

  if (result == NULL) {
    return false;
  }

  // Check the sign first since atojulong() parses only unsigned values.
  if (*value == '-') {
    if (!result->is_intx() && !result->is_int()) {
      return false;
    }
    value++;
    is_neg = true;
  }
  if (!Arguments::atojulong(value, &v)) {
    return false;
  }
  if (result->is_int()) {
    int_v = (int) v;
    if (is_neg) {
      int_v = -int_v;
    }
    return JVMFlag::intAtPut(result, &int_v, origin) == JVMFlag::SUCCESS;
  } else if (result->is_uint()) {
    uint uint_v = (uint) v;
    return JVMFlag::uintAtPut(result, &uint_v, origin) == JVMFlag::SUCCESS;
  } else if (result->is_intx()) {
    intx_v = (intx) v;
    if (is_neg) {
      intx_v = -intx_v;
    }
    return JVMFlag::intxAtPut(result, &intx_v, origin) == JVMFlag::SUCCESS;
  } else if (result->is_uintx()) {
    uintx uintx_v = (uintx) v;
    return JVMFlag::uintxAtPut(result, &uintx_v, origin) == JVMFlag::SUCCESS;
  } else if (result->is_uint64_t()) {
    uint64_t uint64_t_v = (uint64_t) v;
    return JVMFlag::uint64_tAtPut(result, &uint64_t_v, origin) == JVMFlag::SUCCESS;
  } else if (result->is_size_t()) {
    size_t size_t_v = (size_t) v;
    return JVMFlag::size_tAtPut(result, &size_t_v, origin) == JVMFlag::SUCCESS;
  } else if (result->is_double()) {
    double double_v = (double) v;
    return JVMFlag::doubleAtPut(result, &double_v, origin) == JVMFlag::SUCCESS;
  } else {
    return false;
  }
}

static bool set_string_flag(const char* name, const char* value, JVMFlag::Flags origin) {
  if (JVMFlag::ccstrAtPut(name, &value, origin) != JVMFlag::SUCCESS) return false;
  // Contract:  JVMFlag always returns a pointer that needs freeing.
  FREE_C_HEAP_ARRAY(char, value);
  return true;
}

static bool append_to_string_flag(const char* name, const char* new_value, JVMFlag::Flags origin) {
  const char* old_value = "";
  if (JVMFlag::ccstrAt(name, &old_value) != JVMFlag::SUCCESS) return false;
  size_t old_len = old_value != NULL ? strlen(old_value) : 0;
  size_t new_len = strlen(new_value);
  const char* value;
  char* free_this_too = NULL;
  if (old_len == 0) {
    value = new_value;
  } else if (new_len == 0) {
    value = old_value;
  } else {
     size_t length = old_len + 1 + new_len + 1;
     char* buf = NEW_C_HEAP_ARRAY(char, length, mtArguments);
    // each new setting adds another LINE to the switch:
    jio_snprintf(buf, length, "%s\n%s", old_value, new_value);
    value = buf;
    free_this_too = buf;
  }
  (void) JVMFlag::ccstrAtPut(name, &value, origin);
  // JVMFlag always returns a pointer that needs freeing.
  FREE_C_HEAP_ARRAY(char, value);
  if (free_this_too != NULL) {
    // JVMFlag made its own copy, so I must delete my own temp. buffer.
    FREE_C_HEAP_ARRAY(char, free_this_too);
  }
  return true;
}

const char* Arguments::handle_aliases_and_deprecation(const char* arg, bool warn) {
  const char* real_name = real_flag_name(arg);
  JDK_Version since = JDK_Version();
  switch (is_deprecated_flag(arg, &since)) {
    case -1:
      return NULL; // obsolete or expired, don't process normally
    case 0:
      return real_name;
    case 1: {
      if (warn) {
        char version[256];
        since.to_string(version, sizeof(version));
        if (real_name != arg) {
          warning("Option %s was deprecated in version %s and will likely be removed in a future release. Use option %s instead.",
                  arg, version, real_name);
        } else {
          warning("Option %s was deprecated in version %s and will likely be removed in a future release.",
                  arg, version);
        }
      }
      return real_name;
    }
  }
  ShouldNotReachHere();
  return NULL;
}

void log_deprecated_flag(const char* name, bool on, AliasedLoggingFlag alf) {
  LogTagType tagSet[] = {alf.tag0, alf.tag1, alf.tag2, alf.tag3, alf.tag4, alf.tag5};
  // Set tagset string buffer at max size of 256, large enough for any alias tagset
  const int max_tagset_size = 256;
  int max_tagset_len = max_tagset_size - 1;
  char tagset_buffer[max_tagset_size];
  tagset_buffer[0] = '\0';

  // Write tag-set for aliased logging option, in string list form
  int max_tags = sizeof(tagSet)/sizeof(tagSet[0]);
  for (int i = 0; i < max_tags && tagSet[i] != LogTag::__NO_TAG; i++) {
    if (i > 0) {
      strncat(tagset_buffer, "+", max_tagset_len - strlen(tagset_buffer));
    }
    strncat(tagset_buffer, LogTag::name(tagSet[i]), max_tagset_len - strlen(tagset_buffer));
  }
  if (!alf.exactMatch) {
      strncat(tagset_buffer, "*", max_tagset_len - strlen(tagset_buffer));
  }
  log_warning(arguments)("-XX:%s%s is deprecated. Will use -Xlog:%s=%s instead.",
                         (on) ? "+" : "-",
                         name,
                         tagset_buffer,
                         (on) ? LogLevel::name(alf.level) : "off");
}

AliasedLoggingFlag Arguments::catch_logging_aliases(const char* name, bool on){
  for (size_t i = 0; aliased_logging_flags[i].alias_name != NULL; i++) {
    const AliasedLoggingFlag& alf = aliased_logging_flags[i];
    if (strcmp(alf.alias_name, name) == 0) {
      log_deprecated_flag(name, on, alf);
      return alf;
    }
  }
  AliasedLoggingFlag a = {NULL, LogLevel::Off, false, LOG_TAGS(_NO_TAG)};
  return a;
}

bool Arguments::parse_argument(const char* arg, JVMFlag::Flags origin) {

  // range of acceptable characters spelled out for portability reasons
#define NAME_RANGE  "[abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_]"
#define BUFLEN 255
  char name[BUFLEN+1];
  char dummy;
  const char* real_name;
  bool warn_if_deprecated = true;

  if (sscanf(arg, "-%" XSTR(BUFLEN) NAME_RANGE "%c", name, &dummy) == 1) {
    AliasedLoggingFlag alf = catch_logging_aliases(name, false);
    if (alf.alias_name != NULL){
      LogConfiguration::configure_stdout(LogLevel::Off, alf.exactMatch, alf.tag0, alf.tag1, alf.tag2, alf.tag3, alf.tag4, alf.tag5);
      return true;
    }
    real_name = handle_aliases_and_deprecation(name, warn_if_deprecated);
    if (real_name == NULL) {
      return false;
    }
    return set_bool_flag(real_name, false, origin);
  }
  if (sscanf(arg, "+%" XSTR(BUFLEN) NAME_RANGE "%c", name, &dummy) == 1) {
    AliasedLoggingFlag alf = catch_logging_aliases(name, true);
    if (alf.alias_name != NULL){
      LogConfiguration::configure_stdout(alf.level, alf.exactMatch, alf.tag0, alf.tag1, alf.tag2, alf.tag3, alf.tag4, alf.tag5);
      return true;
    }
    real_name = handle_aliases_and_deprecation(name, warn_if_deprecated);
    if (real_name == NULL) {
      return false;
    }
    return set_bool_flag(real_name, true, origin);
  }

  char punct;
  if (sscanf(arg, "%" XSTR(BUFLEN) NAME_RANGE "%c", name, &punct) == 2 && punct == '=') {
    const char* value = strchr(arg, '=') + 1;
    JVMFlag* flag;

    // this scanf pattern matches both strings (handled here) and numbers (handled later))
    AliasedLoggingFlag alf = catch_logging_aliases(name, true);
    if (alf.alias_name != NULL) {
      LogConfiguration::configure_stdout(alf.level, alf.exactMatch, alf.tag0, alf.tag1, alf.tag2, alf.tag3, alf.tag4, alf.tag5);
      return true;
    }
    real_name = handle_aliases_and_deprecation(name, warn_if_deprecated);
    if (real_name == NULL) {
      return false;
    }
    flag = JVMFlag::find_flag(real_name);
    if (flag != NULL && flag->is_ccstr()) {
      if (flag->ccstr_accumulates()) {
        return append_to_string_flag(real_name, value, origin);
      } else {
        if (value[0] == '\0') {
          value = NULL;
        }
        return set_string_flag(real_name, value, origin);
      }
    } else {
      warn_if_deprecated = false; // if arg is deprecated, we've already done warning...
    }
  }

  if (sscanf(arg, "%" XSTR(BUFLEN) NAME_RANGE ":%c", name, &punct) == 2 && punct == '=') {
    const char* value = strchr(arg, '=') + 1;
    // -XX:Foo:=xxx will reset the string flag to the given value.
    if (value[0] == '\0') {
      value = NULL;
    }
    real_name = handle_aliases_and_deprecation(name, warn_if_deprecated);
    if (real_name == NULL) {
      return false;
    }
    return set_string_flag(real_name, value, origin);
  }

#define SIGNED_FP_NUMBER_RANGE "[-0123456789.eE+]"
#define SIGNED_NUMBER_RANGE    "[-0123456789]"
#define        NUMBER_RANGE    "[0123456789eE+-]"
  char value[BUFLEN + 1];
  char value2[BUFLEN + 1];
  if (sscanf(arg, "%" XSTR(BUFLEN) NAME_RANGE "=" "%" XSTR(BUFLEN) SIGNED_NUMBER_RANGE "." "%" XSTR(BUFLEN) NUMBER_RANGE "%c", name, value, value2, &dummy) == 3) {
    // Looks like a floating-point number -- try again with more lenient format string
    if (sscanf(arg, "%" XSTR(BUFLEN) NAME_RANGE "=" "%" XSTR(BUFLEN) SIGNED_FP_NUMBER_RANGE "%c", name, value, &dummy) == 2) {
      real_name = handle_aliases_and_deprecation(name, warn_if_deprecated);
      if (real_name == NULL) {
        return false;
      }
      return set_fp_numeric_flag(real_name, value, origin);
    }
  }

#define VALUE_RANGE "[-kmgtxKMGTX0123456789abcdefABCDEF]"
  if (sscanf(arg, "%" XSTR(BUFLEN) NAME_RANGE "=" "%" XSTR(BUFLEN) VALUE_RANGE "%c", name, value, &dummy) == 2) {
    real_name = handle_aliases_and_deprecation(name, warn_if_deprecated);
    if (real_name == NULL) {
      return false;
    }
    return set_numeric_flag(real_name, value, origin);
  }

  return false;
}

void Arguments::add_string(char*** bldarray, int* count, const char* arg) {
  assert(bldarray != NULL, "illegal argument");

  if (arg == NULL) {
    return;
  }

  int new_count = *count + 1;

  // expand the array and add arg to the last element
  if (*bldarray == NULL) {
    *bldarray = NEW_C_HEAP_ARRAY(char*, new_count, mtArguments);
  } else {
    *bldarray = REALLOC_C_HEAP_ARRAY(char*, *bldarray, new_count, mtArguments);
  }
  (*bldarray)[*count] = os::strdup_check_oom(arg);
  *count = new_count;
}

void Arguments::build_jvm_args(const char* arg) {
  add_string(&_jvm_args_array, &_num_jvm_args, arg);
}

void Arguments::build_jvm_flags(const char* arg) {
  add_string(&_jvm_flags_array, &_num_jvm_flags, arg);
}

// utility function to return a string that concatenates all
// strings in a given char** array
const char* Arguments::build_resource_string(char** args, int count) {
  if (args == NULL || count == 0) {
    return NULL;
  }
  size_t length = 0;
  for (int i = 0; i < count; i++) {
    length += strlen(args[i]) + 1; // add 1 for a space or NULL terminating character
  }
  char* s = NEW_RESOURCE_ARRAY(char, length);
  char* dst = s;
  for (int j = 0; j < count; j++) {
    size_t offset = strlen(args[j]) + 1; // add 1 for a space or NULL terminating character
    jio_snprintf(dst, length, "%s ", args[j]); // jio_snprintf will replace the last space character with NULL character
    dst += offset;
    length -= offset;
  }
  return (const char*) s;
}

void Arguments::print_on(outputStream* st) {
  st->print_cr("VM Arguments:");
  if (num_jvm_flags() > 0) {
    st->print("jvm_flags: "); print_jvm_flags_on(st);
    st->cr();
  }
  if (num_jvm_args() > 0) {
    st->print("jvm_args: "); print_jvm_args_on(st);
    st->cr();
  }
  st->print_cr("java_command: %s", java_command() ? java_command() : "<unknown>");
  if (_java_class_path != NULL) {
    char* path = _java_class_path->value();
    st->print_cr("java_class_path (initial): %s", strlen(path) == 0 ? "<not set>" : path );
  }
  st->print_cr("Launcher Type: %s", _sun_java_launcher);
}

void Arguments::print_summary_on(outputStream* st) {
  // Print the command line.  Environment variables that are helpful for
  // reproducing the problem are written later in the hs_err file.
  // flags are from setting file
  if (num_jvm_flags() > 0) {
    st->print_raw("Settings File: ");
    print_jvm_flags_on(st);
    st->cr();
  }
  // args are the command line and environment variable arguments.
  st->print_raw("Command Line: ");
  if (num_jvm_args() > 0) {
    print_jvm_args_on(st);
  }
  // this is the classfile and any arguments to the java program
  if (java_command() != NULL) {
    st->print("%s", java_command());
  }
  st->cr();
}

void Arguments::print_jvm_flags_on(outputStream* st) {
  if (_num_jvm_flags > 0) {
    for (int i=0; i < _num_jvm_flags; i++) {
      st->print("%s ", _jvm_flags_array[i]);
    }
  }
}

void Arguments::print_jvm_args_on(outputStream* st) {
  if (_num_jvm_args > 0) {
    for (int i=0; i < _num_jvm_args; i++) {
      st->print("%s ", _jvm_args_array[i]);
    }
  }
}

bool Arguments::process_argument(const char* arg,
                                 jboolean ignore_unrecognized,
                                 JVMFlag::Flags origin) {
  JDK_Version since = JDK_Version();

  if (parse_argument(arg, origin)) {
    return true;
  }

  // Determine if the flag has '+', '-', or '=' characters.
  bool has_plus_minus = (*arg == '+' || *arg == '-');
  const char* const argname = has_plus_minus ? arg + 1 : arg;

  size_t arg_len;
  const char* equal_sign = strchr(argname, '=');
  if (equal_sign == NULL) {
    arg_len = strlen(argname);
  } else {
    arg_len = equal_sign - argname;
  }

  // Only make the obsolete check for valid arguments.
  if (arg_len <= BUFLEN) {
    // Construct a string which consists only of the argument name without '+', '-', or '='.
    char stripped_argname[BUFLEN+1]; // +1 for '\0'
    jio_snprintf(stripped_argname, arg_len+1, "%s", argname); // +1 for '\0'
    if (is_obsolete_flag(stripped_argname, &since)) {
      if (strcmp(stripped_argname, "UseAppCDS") != 0) {
        char version[256];
        since.to_string(version, sizeof(version));
        warning("Ignoring option %s; support was removed in %s", stripped_argname, version);
      } else {
        warning("Ignoring obsolete option UseAppCDS; AppCDS is automatically enabled");
      }
      return true;
    }
#ifndef PRODUCT
    else {
      const char* replacement;
      if ((replacement = removed_develop_logging_flag_name(stripped_argname)) != NULL){
        log_warning(arguments)("%s has been removed. Please use %s instead.",
                               stripped_argname,
                               replacement);
        return false;
      }
    }
#endif //PRODUCT
  }

  // For locked flags, report a custom error message if available.
  // Otherwise, report the standard unrecognized VM option.
  JVMFlag* found_flag = JVMFlag::find_flag((const char*)argname, arg_len, true, true);
  if (found_flag != NULL) {
    char locked_message_buf[BUFLEN];
    JVMFlag::MsgType msg_type = found_flag->get_locked_message(locked_message_buf, BUFLEN);
    if (strlen(locked_message_buf) == 0) {
      if (found_flag->is_bool() && !has_plus_minus) {
        jio_fprintf(defaultStream::error_stream(),
          "Missing +/- setting for VM option '%s'\n", argname);
      } else if (!found_flag->is_bool() && has_plus_minus) {
        jio_fprintf(defaultStream::error_stream(),
          "Unexpected +/- setting in VM option '%s'\n", argname);
      } else {
        jio_fprintf(defaultStream::error_stream(),
          "Improperly specified VM option '%s'\n", argname);
      }
    } else {
#ifdef PRODUCT
      bool mismatched = ((msg_type == JVMFlag::NOTPRODUCT_FLAG_BUT_PRODUCT_BUILD) ||
                         (msg_type == JVMFlag::DEVELOPER_FLAG_BUT_PRODUCT_BUILD));
      if (ignore_unrecognized && mismatched) {
        return true;
      }
#endif
      jio_fprintf(defaultStream::error_stream(), "%s", locked_message_buf);
    }
  } else {
    if (ignore_unrecognized) {
      return true;
    }
    jio_fprintf(defaultStream::error_stream(),
                "Unrecognized VM option '%s'\n", argname);
    JVMFlag* fuzzy_matched = JVMFlag::fuzzy_match((const char*)argname, arg_len, true);
    if (fuzzy_matched != NULL) {
      jio_fprintf(defaultStream::error_stream(),
                  "Did you mean '%s%s%s'? ",
                  (fuzzy_matched->is_bool()) ? "(+/-)" : "",
                  fuzzy_matched->_name,
                  (fuzzy_matched->is_bool()) ? "" : "=<value>");
    }
  }

  // allow for commandline "commenting out" options like -XX:#+Verbose
  return arg[0] == '#';
}

bool Arguments::process_settings_file(const char* file_name, bool should_exist, jboolean ignore_unrecognized) {
  FILE* stream = fopen(file_name, "rb");
  if (stream == NULL) {
    if (should_exist) {
      jio_fprintf(defaultStream::error_stream(),
                  "Could not open settings file %s\n", file_name);
      return false;
    } else {
      return true;
    }
  }

  char token[1024];
  int  pos = 0;

  bool in_white_space = true;
  bool in_comment     = false;
  bool in_quote       = false;
  char quote_c        = 0;
  bool result         = true;

  int c = getc(stream);
  while(c != EOF && pos < (int)(sizeof(token)-1)) {
    if (in_white_space) {
      if (in_comment) {
        if (c == '\n') in_comment = false;
      } else {
        if (c == '#') in_comment = true;
        else if (!isspace(c)) {
          in_white_space = false;
          token[pos++] = c;
        }
      }
    } else {
      if (c == '\n' || (!in_quote && isspace(c))) {
        // token ends at newline, or at unquoted whitespace
        // this allows a way to include spaces in string-valued options
        token[pos] = '\0';
        logOption(token);
        result &= process_argument(token, ignore_unrecognized, JVMFlag::CONFIG_FILE);
        build_jvm_flags(token);
        pos = 0;
        in_white_space = true;
        in_quote = false;
      } else if (!in_quote && (c == '\'' || c == '"')) {
        in_quote = true;
        quote_c = c;
      } else if (in_quote && (c == quote_c)) {
        in_quote = false;
      } else {
        token[pos++] = c;
      }
    }
    c = getc(stream);
  }
  if (pos > 0) {
    token[pos] = '\0';
    result &= process_argument(token, ignore_unrecognized, JVMFlag::CONFIG_FILE);
    build_jvm_flags(token);
  }
  fclose(stream);
  return result;
}

//=============================================================================================================
// Parsing of properties (-D)

const char* Arguments::get_property(const char* key) {
  return PropertyList_get_value(system_properties(), key);
}

bool Arguments::add_property(const char* prop, PropertyWriteable writeable, PropertyInternal internal) {
  const char* eq = strchr(prop, '=');
  const char* key;
  const char* value = "";

  if (eq == NULL) {
    // property doesn't have a value, thus use passed string
    key = prop;
  } else {
    // property have a value, thus extract it and save to the
    // allocated string
    size_t key_len = eq - prop;
    char* tmp_key = AllocateHeap(key_len + 1, mtArguments);

    jio_snprintf(tmp_key, key_len + 1, "%s", prop);
    key = tmp_key;

    value = &prop[key_len + 1];
  }

  if (strcmp(key, "java.compiler") == 0) {
    process_java_compiler_argument(value);
    // Record value in Arguments, but let it get passed to Java.
  } else if (strcmp(key, "sun.java.launcher.is_altjvm") == 0 ||
             strcmp(key, "sun.java.launcher.pid") == 0) {
    // sun.java.launcher.is_altjvm and sun.java.launcher.pid property are
    // private and are processed in process_sun_java_launcher_properties();
    // the sun.java.launcher property is passed on to the java application
  } else if (strcmp(key, "sun.boot.library.path") == 0) {
    // append is true, writable is true, internal is false
    PropertyList_unique_add(&_system_properties, key, value, AppendProperty,
                            WriteableProperty, ExternalProperty);
  } else {
    if (strcmp(key, "sun.java.command") == 0) {
      char *old_java_command = _java_command;
      _java_command = os::strdup_check_oom(value, mtArguments);
      if (old_java_command != NULL) {
        os::free(old_java_command);
      }
    } else if (strcmp(key, "java.vendor.url.bug") == 0) {
      // If this property is set on the command line then its value will be
      // displayed in VM error logs as the URL at which to submit such logs.
      // Normally the URL displayed in error logs is different from the value
      // of this system property, so a different property should have been
      // used here, but we leave this as-is in case someone depends upon it.
      const char* old_java_vendor_url_bug = _java_vendor_url_bug;
      // save it in _java_vendor_url_bug, so JVM fatal error handler can access
      // its value without going through the property list or making a Java call.
      _java_vendor_url_bug = os::strdup_check_oom(value, mtArguments);
      if (old_java_vendor_url_bug != NULL) {
        os::free((void *)old_java_vendor_url_bug);
      }
    }

    // Create new property and add at the end of the list
    PropertyList_unique_add(&_system_properties, key, value, AddProperty, writeable, internal);
  }

  if (key != prop) {
    // SystemProperty copy passed value, thus free previously allocated
    // memory
    FreeHeap((void *)key);
  }

  return true;
}

#if INCLUDE_CDS
const char* unsupported_properties[] = { "jdk.module.limitmods",
                                         "jdk.module.upgrade.path",
                                         "jdk.module.patch.0" };
const char* unsupported_options[] = { "--limit-modules",
                                      "--upgrade-module-path",
                                      "--patch-module"
                                    };
void Arguments::check_unsupported_dumping_properties() {
  assert(DumpSharedSpaces, "this function is only used with -Xshare:dump");
  assert(ARRAY_SIZE(unsupported_properties) == ARRAY_SIZE(unsupported_options), "must be");
  // If a vm option is found in the unsupported_options array, vm will exit with an error message.
  SystemProperty* sp = system_properties();
  while (sp != NULL) {
    for (uint i = 0; i < ARRAY_SIZE(unsupported_properties); i++) {
      if (strcmp(sp->key(), unsupported_properties[i]) == 0) {
        vm_exit_during_initialization(
          "Cannot use the following option when dumping the shared archive", unsupported_options[i]);
      }
    }
    sp = sp->next();
  }

  // Check for an exploded module build in use with -Xshare:dump.
  if (!has_jimage()) {
    vm_exit_during_initialization("Dumping the shared archive is not supported with an exploded module build");
  }
}

bool Arguments::check_unsupported_cds_runtime_properties() {
  assert(UseSharedSpaces, "this function is only used with -Xshare:{on,auto}");
  assert(ARRAY_SIZE(unsupported_properties) == ARRAY_SIZE(unsupported_options), "must be");
  for (uint i = 0; i < ARRAY_SIZE(unsupported_properties); i++) {
    if (get_property(unsupported_properties[i]) != NULL) {
      if (RequireSharedSpaces) {
        warning("CDS is disabled when the %s option is specified.", unsupported_options[i]);
      }
      return true;
    }
  }
  return false;
}
#endif

//===========================================================================================================
// Setting int/mixed/comp mode flags

void Arguments::set_mode_flags(Mode mode) {
  // Set up default values for all flags.
  // If you add a flag to any of the branches below,
  // add a default value for it here.
  set_java_compiler(false);
  _mode                      = mode;

  // Ensure Agent_OnLoad has the correct initial values.
  // This may not be the final mode; mode may change later in onload phase.
  PropertyList_unique_add(&_system_properties, "java.vm.info",
                          VM_Version::vm_info_string(), AddProperty, UnwriteableProperty, ExternalProperty);

  UseInterpreter             = true;
  UseCompiler                = true;
  UseLoopCounter             = true;

  // Default values may be platform/compiler dependent -
  // use the saved values
  ClipInlining               = Arguments::_ClipInlining;
  AlwaysCompileLoopMethods   = Arguments::_AlwaysCompileLoopMethods;
  UseOnStackReplacement      = Arguments::_UseOnStackReplacement;
  BackgroundCompilation      = Arguments::_BackgroundCompilation;
  if (TieredCompilation) {
    if (FLAG_IS_DEFAULT(Tier3InvokeNotifyFreqLog)) {
      Tier3InvokeNotifyFreqLog = Arguments::_Tier3InvokeNotifyFreqLog;
    }
    if (FLAG_IS_DEFAULT(Tier4InvocationThreshold)) {
      Tier4InvocationThreshold = Arguments::_Tier4InvocationThreshold;
    }
  }

  // Change from defaults based on mode
  switch (mode) {
  default:
    ShouldNotReachHere();
    break;
  case _int:
    UseCompiler              = false;
    UseLoopCounter           = false;
    AlwaysCompileLoopMethods = false;
    UseOnStackReplacement    = false;
    break;
  case _mixed:
    // same as default
    break;
  case _comp:
    UseInterpreter           = false;
    BackgroundCompilation    = false;
    ClipInlining             = false;
    // Be much more aggressive in tiered mode with -Xcomp and exercise C2 more.
    // We will first compile a level 3 version (C1 with full profiling), then do one invocation of it and
    // compile a level 4 (C2) and then continue executing it.
    if (TieredCompilation) {
      Tier3InvokeNotifyFreqLog = 0;
      Tier4InvocationThreshold = 0;
    }
    break;
  }
}

// Conflict: required to use shared spaces (-Xshare:on), but
// incompatible command line options were chosen.
static void no_shared_spaces(const char* message) {
  if (RequireSharedSpaces) {
    jio_fprintf(defaultStream::error_stream(),
      "Class data sharing is inconsistent with other specified options.\n");
    vm_exit_during_initialization("Unable to use shared archive", message);
  } else {
    FLAG_SET_DEFAULT(UseSharedSpaces, false);
  }
}

void set_object_alignment() {
  // Object alignment.
  assert(is_power_of_2(ObjectAlignmentInBytes), "ObjectAlignmentInBytes must be power of 2");
  MinObjAlignmentInBytes     = ObjectAlignmentInBytes;
  assert(MinObjAlignmentInBytes >= HeapWordsPerLong * HeapWordSize, "ObjectAlignmentInBytes value is too small");
  MinObjAlignment            = MinObjAlignmentInBytes / HeapWordSize;
  assert(MinObjAlignmentInBytes == MinObjAlignment * HeapWordSize, "ObjectAlignmentInBytes value is incorrect");
  MinObjAlignmentInBytesMask = MinObjAlignmentInBytes - 1;

  LogMinObjAlignmentInBytes  = exact_log2(ObjectAlignmentInBytes);
  LogMinObjAlignment         = LogMinObjAlignmentInBytes - LogHeapWordSize;

  // Oop encoding heap max
  OopEncodingHeapMax = (uint64_t(max_juint) + 1) << LogMinObjAlignmentInBytes;

  if (SurvivorAlignmentInBytes == 0) {
    SurvivorAlignmentInBytes = ObjectAlignmentInBytes;
  }
}

size_t Arguments::max_heap_for_compressed_oops() {
  // Avoid sign flip.
  assert(OopEncodingHeapMax > (uint64_t)os::vm_page_size(), "Unusual page size");
  // We need to fit both the NULL page and the heap into the memory budget, while
  // keeping alignment constraints of the heap. To guarantee the latter, as the
  // NULL page is located before the heap, we pad the NULL page to the conservative
  // maximum alignment that the GC may ever impose upon the heap.
  size_t displacement_due_to_null_page = align_up((size_t)os::vm_page_size(),
                                                  _conservative_max_heap_alignment);

  LP64_ONLY(return OopEncodingHeapMax - displacement_due_to_null_page);
  NOT_LP64(ShouldNotReachHere(); return 0);
}

void Arguments::set_use_compressed_oops() {
#ifndef ZERO
#ifdef _LP64
  // MaxHeapSize is not set up properly at this point, but
  // the only value that can override MaxHeapSize if we are
  // to use UseCompressedOops is InitialHeapSize.
  size_t max_heap_size = MAX2(MaxHeapSize, InitialHeapSize);

  if (max_heap_size <= max_heap_for_compressed_oops()) {
#if !defined(COMPILER1) || defined(TIERED)
    if (FLAG_IS_DEFAULT(UseCompressedOops)) {
      FLAG_SET_ERGO(bool, UseCompressedOops, true);
    }
#endif
  } else {
    if (UseCompressedOops && !FLAG_IS_DEFAULT(UseCompressedOops)) {
      warning("Max heap size too large for Compressed Oops");
      FLAG_SET_DEFAULT(UseCompressedOops, false);
      FLAG_SET_DEFAULT(UseCompressedClassPointers, false);
    }
  }
#endif // _LP64
#endif // ZERO
}


// NOTE: set_use_compressed_klass_ptrs() must be called after calling
// set_use_compressed_oops().
void Arguments::set_use_compressed_klass_ptrs() {
#ifndef ZERO
#ifdef _LP64
  // UseCompressedOops must be on for UseCompressedClassPointers to be on.
  if (!UseCompressedOops) {
    if (UseCompressedClassPointers) {
      warning("UseCompressedClassPointers requires UseCompressedOops");
    }
    FLAG_SET_DEFAULT(UseCompressedClassPointers, false);
  } else {
    // Turn on UseCompressedClassPointers too
    if (FLAG_IS_DEFAULT(UseCompressedClassPointers)) {
      FLAG_SET_ERGO(bool, UseCompressedClassPointers, true);
    }
    // Check the CompressedClassSpaceSize to make sure we use compressed klass ptrs.
    if (UseCompressedClassPointers) {
      if (CompressedClassSpaceSize > KlassEncodingMetaspaceMax) {
        warning("CompressedClassSpaceSize is too large for UseCompressedClassPointers");
        FLAG_SET_DEFAULT(UseCompressedClassPointers, false);
      }
    }
  }
#endif // _LP64
#endif // !ZERO
}

void Arguments::set_conservative_max_heap_alignment() {
  // The conservative maximum required alignment for the heap is the maximum of
  // the alignments imposed by several sources: any requirements from the heap
  // itself, the collector policy and the maximum page size we may run the VM
  // with.
  size_t heap_alignment = GCConfig::arguments()->conservative_max_heap_alignment();
  _conservative_max_heap_alignment = MAX4(heap_alignment,
                                          (size_t)os::vm_allocation_granularity(),
                                          os::max_page_size(),
                                          CollectorPolicy::compute_heap_alignment());
}

jint Arguments::set_ergonomics_flags() {
  GCConfig::initialize();

  set_conservative_max_heap_alignment();

#ifndef ZERO
#ifdef _LP64
  set_use_compressed_oops();

  // set_use_compressed_klass_ptrs() must be called after calling
  // set_use_compressed_oops().
  set_use_compressed_klass_ptrs();

  // Also checks that certain machines are slower with compressed oops
  // in vm_version initialization code.
#endif // _LP64
#endif // !ZERO

  return JNI_OK;
}

julong Arguments::limit_by_allocatable_memory(julong limit) {
  julong max_allocatable;
  julong result = limit;
  if (os::has_allocatable_memory_limit(&max_allocatable)) {
    result = MIN2(result, max_allocatable / MaxVirtMemFraction);
  }
  return result;
}

// Use static initialization to get the default before parsing
static const size_t DefaultHeapBaseMinAddress = HeapBaseMinAddress;

void Arguments::set_heap_size() {
  julong phys_mem =
    FLAG_IS_DEFAULT(MaxRAM) ? MIN2(os::physical_memory(), (julong)MaxRAM)
                            : (julong)MaxRAM;

  // Convert deprecated flags
  if (FLAG_IS_DEFAULT(MaxRAMPercentage) &&
      !FLAG_IS_DEFAULT(MaxRAMFraction))
    MaxRAMPercentage = 100.0 / MaxRAMFraction;

  if (FLAG_IS_DEFAULT(MinRAMPercentage) &&
      !FLAG_IS_DEFAULT(MinRAMFraction))
    MinRAMPercentage = 100.0 / MinRAMFraction;

  if (FLAG_IS_DEFAULT(InitialRAMPercentage) &&
      !FLAG_IS_DEFAULT(InitialRAMFraction))
    InitialRAMPercentage = 100.0 / InitialRAMFraction;

  // If the maximum heap size has not been set with -Xmx,
  // then set it as fraction of the size of physical memory,
  // respecting the maximum and minimum sizes of the heap.
  if (FLAG_IS_DEFAULT(MaxHeapSize)) {
    julong reasonable_max = (julong)((phys_mem * MaxRAMPercentage) / 100);
    const julong reasonable_min = (julong)((phys_mem * MinRAMPercentage) / 100);
    if (reasonable_min < MaxHeapSize) {
      // Small physical memory, so use a minimum fraction of it for the heap
      reasonable_max = reasonable_min;
    } else {
      // Not-small physical memory, so require a heap at least
      // as large as MaxHeapSize
      reasonable_max = MAX2(reasonable_max, (julong)MaxHeapSize);
    }

    if (!FLAG_IS_DEFAULT(ErgoHeapSizeLimit) && ErgoHeapSizeLimit != 0) {
      // Limit the heap size to ErgoHeapSizeLimit
      reasonable_max = MIN2(reasonable_max, (julong)ErgoHeapSizeLimit);
    }
    if (UseCompressedOops) {
      // Limit the heap size to the maximum possible when using compressed oops
      julong max_coop_heap = (julong)max_heap_for_compressed_oops();

      // HeapBaseMinAddress can be greater than default but not less than.
      if (!FLAG_IS_DEFAULT(HeapBaseMinAddress)) {
        if (HeapBaseMinAddress < DefaultHeapBaseMinAddress) {
          // matches compressed oops printing flags
          log_debug(gc, heap, coops)("HeapBaseMinAddress must be at least " SIZE_FORMAT
                                     " (" SIZE_FORMAT "G) which is greater than value given " SIZE_FORMAT,
                                     DefaultHeapBaseMinAddress,
                                     DefaultHeapBaseMinAddress/G,
                                     HeapBaseMinAddress);
          FLAG_SET_ERGO(size_t, HeapBaseMinAddress, DefaultHeapBaseMinAddress);
        }
      }

      if (HeapBaseMinAddress + MaxHeapSize < max_coop_heap) {
        // Heap should be above HeapBaseMinAddress to get zero based compressed oops
        // but it should be not less than default MaxHeapSize.
        max_coop_heap -= HeapBaseMinAddress;
      }
      reasonable_max = MIN2(reasonable_max, max_coop_heap);
    }
    reasonable_max = limit_by_allocatable_memory(reasonable_max);

    if (!FLAG_IS_DEFAULT(InitialHeapSize)) {
      // An initial heap size was specified on the command line,
      // so be sure that the maximum size is consistent.  Done
      // after call to limit_by_allocatable_memory because that
      // method might reduce the allocation size.
      reasonable_max = MAX2(reasonable_max, (julong)InitialHeapSize);
    }

    log_trace(gc, heap)("  Maximum heap size " SIZE_FORMAT, (size_t) reasonable_max);
    FLAG_SET_ERGO(size_t, MaxHeapSize, (size_t)reasonable_max);
  }

  // If the minimum or initial heap_size have not been set or requested to be set
  // ergonomically, set them accordingly.
  if (InitialHeapSize == 0 || min_heap_size() == 0) {
    julong reasonable_minimum = (julong)(OldSize + NewSize);

    reasonable_minimum = MIN2(reasonable_minimum, (julong)MaxHeapSize);

    reasonable_minimum = limit_by_allocatable_memory(reasonable_minimum);

    if (InitialHeapSize == 0) {
      julong reasonable_initial = (julong)((phys_mem * InitialRAMPercentage) / 100);

      reasonable_initial = MAX3(reasonable_initial, reasonable_minimum, (julong)min_heap_size());
      reasonable_initial = MIN2(reasonable_initial, (julong)MaxHeapSize);

      reasonable_initial = limit_by_allocatable_memory(reasonable_initial);

      log_trace(gc, heap)("  Initial heap size " SIZE_FORMAT, (size_t)reasonable_initial);
      FLAG_SET_ERGO(size_t, InitialHeapSize, (size_t)reasonable_initial);
    }
    // If the minimum heap size has not been set (via -Xms),
    // synchronize with InitialHeapSize to avoid errors with the default value.
    if (min_heap_size() == 0) {
      set_min_heap_size(MIN2((size_t)reasonable_minimum, InitialHeapSize));
      log_trace(gc, heap)("  Minimum heap size " SIZE_FORMAT, min_heap_size());
    }
  }
}

// This option inspects the machine and attempts to set various
// parameters to be optimal for long-running, memory allocation
// intensive jobs.  It is intended for machines with large
// amounts of cpu and memory.
jint Arguments::set_aggressive_heap_flags() {
  // initHeapSize is needed since _initial_heap_size is 4 bytes on a 32 bit
  // VM, but we may not be able to represent the total physical memory
  // available (like having 8gb of memory on a box but using a 32bit VM).
  // Thus, we need to make sure we're using a julong for intermediate
  // calculations.
  julong initHeapSize;
  julong total_memory = os::physical_memory();

  if (total_memory < (julong) 256 * M) {
    jio_fprintf(defaultStream::error_stream(),
            "You need at least 256mb of memory to use -XX:+AggressiveHeap\n");
    vm_exit(1);
  }

  // The heap size is half of available memory, or (at most)
  // all of possible memory less 160mb (leaving room for the OS
  // when using ISM).  This is the maximum; because adaptive sizing
  // is turned on below, the actual space used may be smaller.

  initHeapSize = MIN2(total_memory / (julong) 2,
          total_memory - (julong) 160 * M);

  initHeapSize = limit_by_allocatable_memory(initHeapSize);

  if (FLAG_IS_DEFAULT(MaxHeapSize)) {
    if (FLAG_SET_CMDLINE(size_t, MaxHeapSize, initHeapSize) != JVMFlag::SUCCESS) {
      return JNI_EINVAL;
    }
    if (FLAG_SET_CMDLINE(size_t, InitialHeapSize, initHeapSize) != JVMFlag::SUCCESS) {
      return JNI_EINVAL;
    }
    // Currently the minimum size and the initial heap sizes are the same.
    set_min_heap_size(initHeapSize);
  }
  if (FLAG_IS_DEFAULT(NewSize)) {
    // Make the young generation 3/8ths of the total heap.
    if (FLAG_SET_CMDLINE(size_t, NewSize,
            ((julong) MaxHeapSize / (julong) 8) * (julong) 3) != JVMFlag::SUCCESS) {
      return JNI_EINVAL;
    }
    if (FLAG_SET_CMDLINE(size_t, MaxNewSize, NewSize) != JVMFlag::SUCCESS) {
      return JNI_EINVAL;
    }
  }

#if !defined(_ALLBSD_SOURCE) && !defined(AIX)  // UseLargePages is not yet supported on BSD and AIX.
  FLAG_SET_DEFAULT(UseLargePages, true);
#endif

  // Increase some data structure sizes for efficiency
  if (FLAG_SET_CMDLINE(size_t, BaseFootPrintEstimate, MaxHeapSize) != JVMFlag::SUCCESS) {
    return JNI_EINVAL;
  }
  if (FLAG_SET_CMDLINE(bool, ResizeTLAB, false) != JVMFlag::SUCCESS) {
    return JNI_EINVAL;
  }
  if (FLAG_SET_CMDLINE(size_t, TLABSize, 256 * K) != JVMFlag::SUCCESS) {
    return JNI_EINVAL;
  }

  // See the OldPLABSize comment below, but replace 'after promotion'
  // with 'after copying'.  YoungPLABSize is the size of the survivor
  // space per-gc-thread buffers.  The default is 4kw.
  if (FLAG_SET_CMDLINE(size_t, YoungPLABSize, 256 * K) != JVMFlag::SUCCESS) { // Note: this is in words
    return JNI_EINVAL;
  }

  // OldPLABSize is the size of the buffers in the old gen that
  // UseParallelGC uses to promote live data that doesn't fit in the
  // survivor spaces.  At any given time, there's one for each gc thread.
  // The default size is 1kw. These buffers are rarely used, since the
  // survivor spaces are usually big enough.  For specjbb, however, there
  // are occasions when there's lots of live data in the young gen
  // and we end up promoting some of it.  We don't have a definite
  // explanation for why bumping OldPLABSize helps, but the theory
  // is that a bigger PLAB results in retaining something like the
  // original allocation order after promotion, which improves mutator
  // locality.  A minor effect may be that larger PLABs reduce the
  // number of PLAB allocation events during gc.  The value of 8kw
  // was arrived at by experimenting with specjbb.
  if (FLAG_SET_CMDLINE(size_t, OldPLABSize, 8 * K) != JVMFlag::SUCCESS) { // Note: this is in words
    return JNI_EINVAL;
  }

  // Enable parallel GC and adaptive generation sizing
  if (FLAG_SET_CMDLINE(bool, UseParallelGC, true) != JVMFlag::SUCCESS) {
    return JNI_EINVAL;
  }

  // Encourage steady state memory management
  if (FLAG_SET_CMDLINE(uintx, ThresholdTolerance, 100) != JVMFlag::SUCCESS) {
    return JNI_EINVAL;
  }

  // This appears to improve mutator locality
  if (FLAG_SET_CMDLINE(bool, ScavengeBeforeFullGC, false) != JVMFlag::SUCCESS) {
    return JNI_EINVAL;
  }

  // Get around early Solaris scheduling bug
  // (affinity vs other jobs on system)
  // but disallow DR and offlining (5008695).
  if (FLAG_SET_CMDLINE(bool, BindGCTaskThreadsToCPUs, true) != JVMFlag::SUCCESS) {
    return JNI_EINVAL;
  }

  return JNI_OK;
}

// This must be called after ergonomics.
void Arguments::set_bytecode_flags() {
  if (!RewriteBytecodes) {
    FLAG_SET_DEFAULT(RewriteFrequentPairs, false);
  }
}

// Aggressive optimization flags  -XX:+AggressiveOpts
jint Arguments::set_aggressive_opts_flags() {
#ifdef COMPILER2
  if (AggressiveUnboxing) {
    if (FLAG_IS_DEFAULT(EliminateAutoBox)) {
      FLAG_SET_DEFAULT(EliminateAutoBox, true);
    } else if (!EliminateAutoBox) {
      // warning("AggressiveUnboxing is disabled because EliminateAutoBox is disabled");
      AggressiveUnboxing = false;
    }
    if (FLAG_IS_DEFAULT(DoEscapeAnalysis)) {
      FLAG_SET_DEFAULT(DoEscapeAnalysis, true);
    } else if (!DoEscapeAnalysis) {
      // warning("AggressiveUnboxing is disabled because DoEscapeAnalysis is disabled");
      AggressiveUnboxing = false;
    }
  }

  // LazyBox need set AggressiveUnboxing
  if (LazyBox) {
    if (!AggressiveUnboxing || UseAOT || EnableJVMCI) {
      warning("LazyBox is disable because AggressiveUnboxing is disabled or UseAOT/EnableJVMCI is enable");
      LazyBox = false;
    }
  }

  if (PrintLazyBox) {
    PrintLazyBox = LazyBox;
  }

  if (AggressiveOpts || !FLAG_IS_DEFAULT(AutoBoxCacheMax)) {
    if (FLAG_IS_DEFAULT(EliminateAutoBox)) {
      FLAG_SET_DEFAULT(EliminateAutoBox, true);
    }
    if (FLAG_IS_DEFAULT(AutoBoxCacheMax)) {
      FLAG_SET_DEFAULT(AutoBoxCacheMax, 20000);
    }

    // Feed the cache size setting into the JDK
    char buffer[1024];
    jio_snprintf(buffer, 1024, "java.lang.Integer.IntegerCache.high=" INTX_FORMAT, AutoBoxCacheMax);
    if (!add_property(buffer)) {
      return JNI_ENOMEM;
    }
  }
  if (AggressiveOpts && FLAG_IS_DEFAULT(BiasedLockingStartupDelay)) {
    FLAG_SET_DEFAULT(BiasedLockingStartupDelay, 500);
  }
#endif

  if (AggressiveOpts) {
// Sample flag setting code
//    if (FLAG_IS_DEFAULT(EliminateZeroing)) {
//      FLAG_SET_DEFAULT(EliminateZeroing, true);
//    }
  }

  return JNI_OK;
}

//===========================================================================================================
// Parsing of java.compiler property

void Arguments::process_java_compiler_argument(const char* arg) {
  // For backwards compatibility, Djava.compiler=NONE or ""
  // causes us to switch to -Xint mode UNLESS -Xdebug
  // is also specified.
  if (strlen(arg) == 0 || strcasecmp(arg, "NONE") == 0) {
    set_java_compiler(true);    // "-Djava.compiler[=...]" most recently seen.
  }
}

void Arguments::process_java_launcher_argument(const char* launcher, void* extra_info) {
  _sun_java_launcher = os::strdup_check_oom(launcher);
}

bool Arguments::created_by_java_launcher() {
  assert(_sun_java_launcher != NULL, "property must have value");
  return strcmp(DEFAULT_JAVA_LAUNCHER, _sun_java_launcher) != 0;
}

bool Arguments::sun_java_launcher_is_altjvm() {
  return _sun_java_launcher_is_altjvm;
}

//===========================================================================================================
// Parsing of main arguments

unsigned int addreads_count = 0;
unsigned int addexports_count = 0;
unsigned int addopens_count = 0;
unsigned int addmods_count = 0;
unsigned int patch_mod_count = 0;

// Check the consistency of vm_init_args
bool Arguments::check_vm_args_consistency() {
  // Method for adding checks for flag consistency.
  // The intent is to warn the user of all possible conflicts,
  // before returning an error.
  // Note: Needs platform-dependent factoring.
  bool status = true;

  if (TLABRefillWasteFraction == 0) {
    jio_fprintf(defaultStream::error_stream(),
                "TLABRefillWasteFraction should be a denominator, "
                "not " SIZE_FORMAT "\n",
                TLABRefillWasteFraction);
    status = false;
  }

  if (PrintNMTStatistics) {
#if INCLUDE_NMT
    if (MemTracker::tracking_level() == NMT_off) {
#endif // INCLUDE_NMT
      warning("PrintNMTStatistics is disabled, because native memory tracking is not enabled");
      PrintNMTStatistics = false;
#if INCLUDE_NMT
    }
#endif
  }

  status = CompilerConfig::check_args_consistency(status);
#if INCLUDE_JVMCI
  if (status && EnableJVMCI) {
    PropertyList_unique_add(&_system_properties, "jdk.internal.vm.ci.enabled", "true",
        AddProperty, UnwriteableProperty, InternalProperty);
    if (!create_numbered_module_property("jdk.module.addmods", "jdk.internal.vm.ci", addmods_count++)) {
      return false;
    }
  }
#endif

#ifndef SUPPORT_RESERVED_STACK_AREA
  if (StackReservedPages != 0) {
    FLAG_SET_CMDLINE(intx, StackReservedPages, 0);
    warning("Reserved Stack Area not supported on this platform");
  }
#endif

  if (!FLAG_IS_DEFAULT(AllocateHeapAt)) {
    if ((UseNUMAInterleaving && !FLAG_IS_DEFAULT(UseNUMAInterleaving)) || (UseNUMA && !FLAG_IS_DEFAULT(UseNUMA))) {
      log_warning(arguments) ("NUMA support for Heap depends on the file system when AllocateHeapAt option is used.\n");
    }
  }
  return status;
}

bool Arguments::is_bad_option(const JavaVMOption* option, jboolean ignore,
  const char* option_type) {
  if (ignore) return false;

  const char* spacer = " ";
  if (option_type == NULL) {
    option_type = ++spacer; // Set both to the empty string.
  }

  if (os::obsolete_option(option)) {
    jio_fprintf(defaultStream::error_stream(),
                "Obsolete %s%soption: %s\n", option_type, spacer,
      option->optionString);
    return false;
  } else {
    jio_fprintf(defaultStream::error_stream(),
                "Unrecognized %s%soption: %s\n", option_type, spacer,
      option->optionString);
    return true;
  }
}

static const char* user_assertion_options[] = {
  "-da", "-ea", "-disableassertions", "-enableassertions", 0
};

static const char* system_assertion_options[] = {
  "-dsa", "-esa", "-disablesystemassertions", "-enablesystemassertions", 0
};

bool Arguments::parse_uintx(const char* value,
                            uintx* uintx_arg,
                            uintx min_size) {

  // Check the sign first since atojulong() parses only unsigned values.
  bool value_is_positive = !(*value == '-');

  if (value_is_positive) {
    julong n;
    bool good_return = atojulong(value, &n);
    if (good_return) {
      bool above_minimum = n >= min_size;
      bool value_is_too_large = n > max_uintx;

      if (above_minimum && !value_is_too_large) {
        *uintx_arg = n;
        return true;
      }
    }
  }
  return false;
}

bool Arguments::create_module_property(const char* prop_name, const char* prop_value, PropertyInternal internal) {
  assert(is_internal_module_property(prop_name) ||
         strcmp(prop_name, "jdk.module.illegalAccess") == 0, "unknown module property: '%s'", prop_name);
  size_t prop_len = strlen(prop_name) + strlen(prop_value) + 2;
  char* property = AllocateHeap(prop_len, mtArguments);
  int ret = jio_snprintf(property, prop_len, "%s=%s", prop_name, prop_value);
  if (ret < 0 || ret >= (int)prop_len) {
    FreeHeap(property);
    return false;
  }
  // These are not strictly writeable properties as they cannot be set via -Dprop=val. But that
  // is enforced by checking is_internal_module_property(). We need the property to be writeable so
  // that multiple occurrences of the associated flag just causes the existing property value to be
  // replaced ("last option wins"). Otherwise we would need to keep track of the flags and only convert
  // to a property after we have finished flag processing.
  bool added = add_property(property, WriteableProperty, internal);
  FreeHeap(property);
  return added;
}

bool Arguments::create_numbered_module_property(const char* prop_base_name, const char* prop_value, unsigned int count) {
  assert(is_internal_module_property(prop_base_name), "unknown module property: '%s'", prop_base_name);
  const unsigned int props_count_limit = 1000;
  const int max_digits = 3;
  const int extra_symbols_count = 3; // includes '.', '=', '\0'

  // Make sure count is < props_count_limit. Otherwise, memory allocation will be too small.
  if (count < props_count_limit) {
    size_t prop_len = strlen(prop_base_name) + strlen(prop_value) + max_digits + extra_symbols_count;
    char* property = AllocateHeap(prop_len, mtArguments);
    int ret = jio_snprintf(property, prop_len, "%s.%d=%s", prop_base_name, count, prop_value);
    if (ret < 0 || ret >= (int)prop_len) {
      FreeHeap(property);
      jio_fprintf(defaultStream::error_stream(), "Failed to create property %s.%d=%s\n", prop_base_name, count, prop_value);
      return false;
    }
    bool added = add_property(property, UnwriteableProperty, InternalProperty);
    FreeHeap(property);
    return added;
  }

  jio_fprintf(defaultStream::error_stream(), "Property count limit exceeded: %s, limit=%d\n", prop_base_name, props_count_limit);
  return false;
}

Arguments::ArgsRange Arguments::parse_memory_size(const char* s,
                                                  julong* long_arg,
                                                  julong min_size,
                                                  julong max_size) {
  if (!atojulong(s, long_arg)) return arg_unreadable;
  return check_memory_size(*long_arg, min_size, max_size);
}

// Parse JavaVMInitArgs structure

jint Arguments::parse_vm_init_args(const JavaVMInitArgs *vm_options_args,
                                   const JavaVMInitArgs *java_tool_options_args,
                                   const JavaVMInitArgs *java_options_args,
                                   const JavaVMInitArgs *cmd_line_args) {
  bool patch_mod_javabase = false;

  // Save default settings for some mode flags
  Arguments::_AlwaysCompileLoopMethods = AlwaysCompileLoopMethods;
  Arguments::_UseOnStackReplacement    = UseOnStackReplacement;
  Arguments::_ClipInlining             = ClipInlining;
  Arguments::_BackgroundCompilation    = BackgroundCompilation;
  if (TieredCompilation) {
    Arguments::_Tier3InvokeNotifyFreqLog = Tier3InvokeNotifyFreqLog;
    Arguments::_Tier4InvocationThreshold = Tier4InvocationThreshold;
  }

  // Setup flags for mixed which is the default
  set_mode_flags(_mixed);

  // Parse args structure generated from java.base vm options resource
  jint result = parse_each_vm_init_arg(vm_options_args, &patch_mod_javabase, JVMFlag::JIMAGE_RESOURCE);
  if (result != JNI_OK) {
    return result;
  }

  // Parse args structure generated from JAVA_TOOL_OPTIONS environment
  // variable (if present).
  result = parse_each_vm_init_arg(java_tool_options_args, &patch_mod_javabase, JVMFlag::ENVIRON_VAR);
  if (result != JNI_OK) {
    return result;
  }

  // Parse args structure generated from the command line flags.
  result = parse_each_vm_init_arg(cmd_line_args, &patch_mod_javabase, JVMFlag::COMMAND_LINE);
  if (result != JNI_OK) {
    return result;
  }

  // Parse args structure generated from the _JAVA_OPTIONS environment
  // variable (if present) (mimics classic VM)
  result = parse_each_vm_init_arg(java_options_args, &patch_mod_javabase, JVMFlag::ENVIRON_VAR);
  if (result != JNI_OK) {
    return result;
  }

  // We need to ensure processor and memory resources have been properly
  // configured - which may rely on arguments we just processed - before
  // doing the final argument processing. Any argument processing that
  // needs to know about processor and memory resources must occur after
  // this point.

  os::init_container_support();

  // Do final processing now that all arguments have been parsed
  result = finalize_vm_init_args(patch_mod_javabase);
  if (result != JNI_OK) {
    return result;
  }

  return JNI_OK;
}

// Checks if name in command-line argument -agent{lib,path}:name[=options]
// represents a valid JDWP agent.  is_path==true denotes that we
// are dealing with -agentpath (case where name is a path), otherwise with
// -agentlib
bool valid_jdwp_agent(char *name, bool is_path) {
  char *_name;
  const char *_jdwp = "jdwp";
  size_t _len_jdwp, _len_prefix;

  if (is_path) {
    if ((_name = strrchr(name, (int) *os::file_separator())) == NULL) {
      return false;
    }

    _name++;  // skip past last path separator
    _len_prefix = strlen(JNI_LIB_PREFIX);

    if (strncmp(_name, JNI_LIB_PREFIX, _len_prefix) != 0) {
      return false;
    }

    _name += _len_prefix;
    _len_jdwp = strlen(_jdwp);

    if (strncmp(_name, _jdwp, _len_jdwp) == 0) {
      _name += _len_jdwp;
    }
    else {
      return false;
    }

    if (strcmp(_name, JNI_LIB_SUFFIX) != 0) {
      return false;
    }

    return true;
  }

  if (strcmp(name, _jdwp) == 0) {
    return true;
  }

  return false;
}

int Arguments::process_patch_mod_option(const char* patch_mod_tail, bool* patch_mod_javabase) {
  // --patch-module=<module>=<file>(<pathsep><file>)*
  assert(patch_mod_tail != NULL, "Unexpected NULL patch-module value");
  // Find the equal sign between the module name and the path specification
  const char* module_equal = strchr(patch_mod_tail, '=');
  if (module_equal == NULL) {
    jio_fprintf(defaultStream::output_stream(), "Missing '=' in --patch-module specification\n");
    return JNI_ERR;
  } else {
    // Pick out the module name
    size_t module_len = module_equal - patch_mod_tail;
    char* module_name = NEW_C_HEAP_ARRAY_RETURN_NULL(char, module_len+1, mtArguments);
    if (module_name != NULL) {
      memcpy(module_name, patch_mod_tail, module_len);
      *(module_name + module_len) = '\0';
      // The path piece begins one past the module_equal sign
      add_patch_mod_prefix(module_name, module_equal + 1, patch_mod_javabase);
      FREE_C_HEAP_ARRAY(char, module_name);
      if (!create_numbered_module_property("jdk.module.patch", patch_mod_tail, patch_mod_count++)) {
        return JNI_ENOMEM;
      }
    } else {
      return JNI_ENOMEM;
    }
  }
  return JNI_OK;
}

// Parse -Xss memory string parameter and convert to ThreadStackSize in K.
jint Arguments::parse_xss(const JavaVMOption* option, const char* tail, intx* out_ThreadStackSize) {
  // The min and max sizes match the values in globals.hpp, but scaled
  // with K. The values have been chosen so that alignment with page
  // size doesn't change the max value, which makes the conversions
  // back and forth between Xss value and ThreadStackSize value easier.
  // The values have also been chosen to fit inside a 32-bit signed type.
  const julong min_ThreadStackSize = 0;
  const julong max_ThreadStackSize = 1 * M;

  const julong min_size = min_ThreadStackSize * K;
  const julong max_size = max_ThreadStackSize * K;

  assert(is_aligned(max_size, os::vm_page_size()), "Implementation assumption");

  julong size = 0;
  ArgsRange errcode = parse_memory_size(tail, &size, min_size, max_size);
  if (errcode != arg_in_range) {
    bool silent = (option == NULL); // Allow testing to silence error messages
    if (!silent) {
      jio_fprintf(defaultStream::error_stream(),
                  "Invalid thread stack size: %s\n", option->optionString);
      describe_range_error(errcode);
    }
    return JNI_EINVAL;
  }

  // Internally track ThreadStackSize in units of 1024 bytes.
  const julong size_aligned = align_up(size, K);
  assert(size <= size_aligned,
         "Overflow: " JULONG_FORMAT " " JULONG_FORMAT,
         size, size_aligned);

  const julong size_in_K = size_aligned / K;
  assert(size_in_K < (julong)max_intx,
         "size_in_K doesn't fit in the type of ThreadStackSize: " JULONG_FORMAT,
         size_in_K);

  // Check that code expanding ThreadStackSize to a page aligned number of bytes won't overflow.
  const julong max_expanded = align_up(size_in_K * K, os::vm_page_size());
  assert(max_expanded < max_uintx && max_expanded >= size_in_K,
         "Expansion overflowed: " JULONG_FORMAT " " JULONG_FORMAT,
         max_expanded, size_in_K);

  *out_ThreadStackSize = (intx)size_in_K;

  return JNI_OK;
}

jint Arguments::parse_each_vm_init_arg(const JavaVMInitArgs* args, bool* patch_mod_javabase, JVMFlag::Flags origin) {
  // For match_option to return remaining or value part of option string
  const char* tail;

  // iterate over arguments
  for (int index = 0; index < args->nOptions; index++) {
    bool is_absolute_path = false;  // for -agentpath vs -agentlib

    const JavaVMOption* option = args->options + index;

    if (!match_option(option, "-Djava.class.path", &tail) &&
        !match_option(option, "-Dsun.java.command", &tail) &&
        !match_option(option, "-Dsun.java.launcher", &tail)) {

        // add all jvm options to the jvm_args string. This string
        // is used later to set the java.vm.args PerfData string constant.
        // the -Djava.class.path and the -Dsun.java.command options are
        // omitted from jvm_args string as each have their own PerfData
        // string constant object.
        build_jvm_args(option->optionString);
    }

    // -verbose:[class/module/gc/jni]
    if (match_option(option, "-verbose", &tail)) {
      if (!strcmp(tail, ":class") || !strcmp(tail, "")) {
        LogConfiguration::configure_stdout(LogLevel::Info, true, LOG_TAGS(class, load));
        LogConfiguration::configure_stdout(LogLevel::Info, true, LOG_TAGS(class, unload));
      } else if (!strcmp(tail, ":module")) {
        LogConfiguration::configure_stdout(LogLevel::Info, true, LOG_TAGS(module, load));
        LogConfiguration::configure_stdout(LogLevel::Info, true, LOG_TAGS(module, unload));
      } else if (!strcmp(tail, ":gc")) {
        LogConfiguration::configure_stdout(LogLevel::Info, true, LOG_TAGS(gc));
      } else if (!strcmp(tail, ":jni")) {
        if (FLAG_SET_CMDLINE(bool, PrintJNIResolving, true) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
      }
    // -da / -ea / -disableassertions / -enableassertions
    // These accept an optional class/package name separated by a colon, e.g.,
    // -da:java.lang.Thread.
    } else if (match_option(option, user_assertion_options, &tail, true)) {
      bool enable = option->optionString[1] == 'e';     // char after '-' is 'e'
      if (*tail == '\0') {
        JavaAssertions::setUserClassDefault(enable);
      } else {
        assert(*tail == ':', "bogus match by match_option()");
        JavaAssertions::addOption(tail + 1, enable);
      }
    // -dsa / -esa / -disablesystemassertions / -enablesystemassertions
    } else if (match_option(option, system_assertion_options, &tail, false)) {
      bool enable = option->optionString[1] == 'e';     // char after '-' is 'e'
      JavaAssertions::setSystemClassDefault(enable);
    // -bootclasspath:
    } else if (match_option(option, "-Xbootclasspath:", &tail)) {
        jio_fprintf(defaultStream::output_stream(),
          "-Xbootclasspath is no longer a supported option.\n");
        return JNI_EINVAL;
    // -bootclasspath/a:
    } else if (match_option(option, "-Xbootclasspath/a:", &tail)) {
      Arguments::append_sysclasspath(tail);
    // -bootclasspath/p:
    } else if (match_option(option, "-Xbootclasspath/p:", &tail)) {
        jio_fprintf(defaultStream::output_stream(),
          "-Xbootclasspath/p is no longer a supported option.\n");
        return JNI_EINVAL;
    // -Xrun
    } else if (match_option(option, "-Xrun", &tail)) {
      if (tail != NULL) {
        const char* pos = strchr(tail, ':');
        size_t len = (pos == NULL) ? strlen(tail) : pos - tail;
        char* name = NEW_C_HEAP_ARRAY(char, len + 1, mtArguments);
        jio_snprintf(name, len + 1, "%s", tail);

        char *options = NULL;
        if(pos != NULL) {
          size_t len2 = strlen(pos+1) + 1; // options start after ':'.  Final zero must be copied.
          options = (char*)memcpy(NEW_C_HEAP_ARRAY(char, len2, mtArguments), pos+1, len2);
        }
#if !INCLUDE_JVMTI
        if (strcmp(name, "jdwp") == 0) {
          jio_fprintf(defaultStream::error_stream(),
            "Debugging agents are not supported in this VM\n");
          return JNI_ERR;
        }
#endif // !INCLUDE_JVMTI
        add_init_library(name, options);
      }
    } else if (match_option(option, "--add-reads=", &tail)) {
      if (!create_numbered_module_property("jdk.module.addreads", tail, addreads_count++)) {
        return JNI_ENOMEM;
      }
    } else if (match_option(option, "--add-exports=", &tail)) {
      if (!create_numbered_module_property("jdk.module.addexports", tail, addexports_count++)) {
        return JNI_ENOMEM;
      }
    } else if (match_option(option, "--add-opens=", &tail)) {
      if (!create_numbered_module_property("jdk.module.addopens", tail, addopens_count++)) {
        return JNI_ENOMEM;
      }
    } else if (match_option(option, "--add-modules=", &tail)) {
      if (!create_numbered_module_property("jdk.module.addmods", tail, addmods_count++)) {
        return JNI_ENOMEM;
      }
    } else if (match_option(option, "--limit-modules=", &tail)) {
      if (!create_module_property("jdk.module.limitmods", tail, InternalProperty)) {
        return JNI_ENOMEM;
      }
    } else if (match_option(option, "--module-path=", &tail)) {
      if (!create_module_property("jdk.module.path", tail, ExternalProperty)) {
        return JNI_ENOMEM;
      }
    } else if (match_option(option, "--upgrade-module-path=", &tail)) {
      if (!create_module_property("jdk.module.upgrade.path", tail, ExternalProperty)) {
        return JNI_ENOMEM;
      }
    } else if (match_option(option, "--patch-module=", &tail)) {
      // --patch-module=<module>=<file>(<pathsep><file>)*
      int res = process_patch_mod_option(tail, patch_mod_javabase);
      if (res != JNI_OK) {
        return res;
      }
    } else if (match_option(option, "--illegal-access=", &tail)) {
      if (!create_module_property("jdk.module.illegalAccess", tail, ExternalProperty)) {
        return JNI_ENOMEM;
      }
    // -agentlib and -agentpath
    } else if (match_option(option, "-agentlib:", &tail) ||
          (is_absolute_path = match_option(option, "-agentpath:", &tail))) {
      if(tail != NULL) {
        const char* pos = strchr(tail, '=');
        char* name;
        if (pos == NULL) {
          name = os::strdup_check_oom(tail, mtArguments);
        } else {
          size_t len = pos - tail;
          name = NEW_C_HEAP_ARRAY(char, len + 1, mtArguments);
          memcpy(name, tail, len);
          name[len] = '\0';
        }

        char *options = NULL;
        if(pos != NULL) {
          options = os::strdup_check_oom(pos + 1, mtArguments);
        }
#if !INCLUDE_JVMTI
        if (valid_jdwp_agent(name, is_absolute_path)) {
          jio_fprintf(defaultStream::error_stream(),
            "Debugging agents are not supported in this VM\n");
          return JNI_ERR;
        }
#endif // !INCLUDE_JVMTI
        add_init_agent(name, options, is_absolute_path);
      }
    // -javaagent
    } else if (match_option(option, "-javaagent:", &tail)) {
#if !INCLUDE_JVMTI
      jio_fprintf(defaultStream::error_stream(),
        "Instrumentation agents are not supported in this VM\n");
      return JNI_ERR;
#else
      if (tail != NULL) {
        size_t length = strlen(tail) + 1;
        char *options = NEW_C_HEAP_ARRAY(char, length, mtArguments);
        jio_snprintf(options, length, "%s", tail);
        add_instrument_agent("instrument", options, false);
        // java agents need module java.instrument
        if (!create_numbered_module_property("jdk.module.addmods", "java.instrument", addmods_count++)) {
          return JNI_ENOMEM;
        }
      }
#endif // !INCLUDE_JVMTI
    // --enable_preview
    } else if (match_option(option, "--enable-preview")) {
      set_enable_preview();
    // -Xnoclassgc
    } else if (match_option(option, "-Xnoclassgc")) {
      if (FLAG_SET_CMDLINE(bool, ClassUnloading, false) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    // -Xconcgc
    } else if (match_option(option, "-Xconcgc")) {
      if (FLAG_SET_CMDLINE(bool, UseConcMarkSweepGC, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      handle_extra_cms_flags("-Xconcgc uses UseConcMarkSweepGC");
    // -Xnoconcgc
    } else if (match_option(option, "-Xnoconcgc")) {
      if (FLAG_SET_CMDLINE(bool, UseConcMarkSweepGC, false) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      handle_extra_cms_flags("-Xnoconcgc uses UseConcMarkSweepGC");
    // -Xbatch
    } else if (match_option(option, "-Xbatch")) {
      if (FLAG_SET_CMDLINE(bool, BackgroundCompilation, false) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    // -Xmn for compatibility with other JVM vendors
    } else if (match_option(option, "-Xmn", &tail)) {
      julong long_initial_young_size = 0;
      ArgsRange errcode = parse_memory_size(tail, &long_initial_young_size, 1);
      if (errcode != arg_in_range) {
        jio_fprintf(defaultStream::error_stream(),
                    "Invalid initial young generation size: %s\n", option->optionString);
        describe_range_error(errcode);
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(size_t, MaxNewSize, (size_t)long_initial_young_size) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(size_t, NewSize, (size_t)long_initial_young_size) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    // -Xms
    } else if (match_option(option, "-Xms", &tail)) {
      julong long_initial_heap_size = 0;
      // an initial heap size of 0 means automatically determine
      ArgsRange errcode = parse_memory_size(tail, &long_initial_heap_size, 0);
      if (errcode != arg_in_range) {
        jio_fprintf(defaultStream::error_stream(),
                    "Invalid initial heap size: %s\n", option->optionString);
        describe_range_error(errcode);
        return JNI_EINVAL;
      }
      set_min_heap_size((size_t)long_initial_heap_size);
      // Currently the minimum size and the initial heap sizes are the same.
      // Can be overridden with -XX:InitialHeapSize.
      if (FLAG_SET_CMDLINE(size_t, InitialHeapSize, (size_t)long_initial_heap_size) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    // -Xmx
    } else if (match_option(option, "-Xmx", &tail) || match_option(option, "-XX:MaxHeapSize=", &tail)) {
      julong long_max_heap_size = 0;
      ArgsRange errcode = parse_memory_size(tail, &long_max_heap_size, 1);
      if (errcode != arg_in_range) {
        jio_fprintf(defaultStream::error_stream(),
                    "Invalid maximum heap size: %s\n", option->optionString);
        describe_range_error(errcode);
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(size_t, MaxHeapSize, (size_t)long_max_heap_size) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    // Xmaxf
    } else if (match_option(option, "-Xmaxf", &tail)) {
      char* err;
      int maxf = (int)(strtod(tail, &err) * 100);
      if (*err != '\0' || *tail == '\0') {
        jio_fprintf(defaultStream::error_stream(),
                    "Bad max heap free percentage size: %s\n",
                    option->optionString);
        return JNI_EINVAL;
      } else {
        if (FLAG_SET_CMDLINE(uintx, MaxHeapFreeRatio, maxf) != JVMFlag::SUCCESS) {
            return JNI_EINVAL;
        }
      }
    // Xminf
    } else if (match_option(option, "-Xminf", &tail)) {
      char* err;
      int minf = (int)(strtod(tail, &err) * 100);
      if (*err != '\0' || *tail == '\0') {
        jio_fprintf(defaultStream::error_stream(),
                    "Bad min heap free percentage size: %s\n",
                    option->optionString);
        return JNI_EINVAL;
      } else {
        if (FLAG_SET_CMDLINE(uintx, MinHeapFreeRatio, minf) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
      }
    // -Xss
    } else if (match_option(option, "-Xss", &tail)) {
      intx value = 0;
      jint err = parse_xss(option, tail, &value);
      if (err != JNI_OK) {
        return err;
      }
      if (FLAG_SET_CMDLINE(intx, ThreadStackSize, value) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    } else if (match_option(option, "-Xmaxjitcodesize", &tail) ||
               match_option(option, "-XX:ReservedCodeCacheSize=", &tail)) {
      julong long_ReservedCodeCacheSize = 0;

      ArgsRange errcode = parse_memory_size(tail, &long_ReservedCodeCacheSize, 1);
      if (errcode != arg_in_range) {
        jio_fprintf(defaultStream::error_stream(),
                    "Invalid maximum code cache size: %s.\n", option->optionString);
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(uintx, ReservedCodeCacheSize, (uintx)long_ReservedCodeCacheSize) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    // -green
    } else if (match_option(option, "-green")) {
      jio_fprintf(defaultStream::error_stream(),
                  "Green threads support not available\n");
          return JNI_EINVAL;
    // -native
    } else if (match_option(option, "-native")) {
          // HotSpot always uses native threads, ignore silently for compatibility
    // -Xrs
    } else if (match_option(option, "-Xrs")) {
          // Classic/EVM option, new functionality
      if (FLAG_SET_CMDLINE(bool, ReduceSignalUsage, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      // -Xprof
    } else if (match_option(option, "-Xprof")) {
      char version[256];
      // Obsolete in JDK 10
      JDK_Version::jdk(10).to_string(version, sizeof(version));
      warning("Ignoring option %s; support was removed in %s", option->optionString, version);
    // -Xconcurrentio
    } else if (match_option(option, "-Xconcurrentio")) {
      if (FLAG_SET_CMDLINE(bool, UseLWPSynchronization, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(bool, BackgroundCompilation, false) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      SafepointSynchronize::set_defer_thr_suspend_loop_count();
      if (FLAG_SET_CMDLINE(bool, UseTLAB, false) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(size_t, NewSizeThreadIncrease, 16 * K) != JVMFlag::SUCCESS) {  // 20Kb per thread added to new generation
        return JNI_EINVAL;
      }

      // -Xinternalversion
    } else if (match_option(option, "-Xinternalversion")) {
      jio_fprintf(defaultStream::output_stream(), "%s\n",
                  VM_Version::internal_vm_info_string());
      vm_exit(0);
#ifndef PRODUCT
    // -Xprintflags
    } else if (match_option(option, "-Xprintflags")) {
      JVMFlag::printFlags(tty, false);
      vm_exit(0);
#endif
    // -D
    } else if (match_option(option, "-D", &tail)) {
      const char* value;
      if (match_option(option, "-Djava.endorsed.dirs=", &value) &&
            *value!= '\0' && strcmp(value, "\"\"") != 0) {
        // abort if -Djava.endorsed.dirs is set
        jio_fprintf(defaultStream::output_stream(),
          "-Djava.endorsed.dirs=%s is not supported. Endorsed standards and standalone APIs\n"
          "in modular form will be supported via the concept of upgradeable modules.\n", value);
        return JNI_EINVAL;
      }
      if (match_option(option, "-Djava.ext.dirs=", &value) &&
            *value != '\0' && strcmp(value, "\"\"") != 0) {
        // abort if -Djava.ext.dirs is set
        jio_fprintf(defaultStream::output_stream(),
          "-Djava.ext.dirs=%s is not supported.  Use -classpath instead.\n", value);
        return JNI_EINVAL;
      }
      // Check for module related properties.  They must be set using the modules
      // options. For example: use "--add-modules=java.sql", not
      // "-Djdk.module.addmods=java.sql"
      if (is_internal_module_property(option->optionString + 2)) {
        needs_module_property_warning = true;
        continue;
      }
      if (!add_property(tail)) {
        return JNI_ENOMEM;
      }
      // Out of the box management support
      if (match_option(option, "-Dcom.sun.management", &tail)) {
#if INCLUDE_MANAGEMENT
        if (FLAG_SET_CMDLINE(bool, ManagementServer, true) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
        // management agent in module jdk.management.agent
        if (!create_numbered_module_property("jdk.module.addmods", "jdk.management.agent", addmods_count++)) {
          return JNI_ENOMEM;
        }
#else
        jio_fprintf(defaultStream::output_stream(),
          "-Dcom.sun.management is not supported in this VM.\n");
        return JNI_ERR;
#endif
      }
    // -Xint
    } else if (match_option(option, "-Xint")) {
          set_mode_flags(_int);
    // -Xmixed
    } else if (match_option(option, "-Xmixed")) {
          set_mode_flags(_mixed);
    // -Xcomp
    } else if (match_option(option, "-Xcomp")) {
      // for testing the compiler; turn off all flags that inhibit compilation
          set_mode_flags(_comp);
    // -Xshare:dump
    } else if (match_option(option, "-Xshare:dump")) {
      if (FLAG_SET_CMDLINE(bool, DumpSharedSpaces, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      set_mode_flags(_int);     // Prevent compilation, which creates objects
    // -Xshare:on
    } else if (match_option(option, "-Xshare:on")) {
      if (FLAG_SET_CMDLINE(bool, UseSharedSpaces, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(bool, RequireSharedSpaces, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    // -Xshare:auto
    } else if (match_option(option, "-Xshare:auto")) {
      if (FLAG_SET_CMDLINE(bool, UseSharedSpaces, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(bool, RequireSharedSpaces, false) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    // -Xshare:off
    } else if (match_option(option, "-Xshare:off")) {
      if (FLAG_SET_CMDLINE(bool, UseSharedSpaces, false) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(bool, RequireSharedSpaces, false) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    // -Xverify
    } else if (match_option(option, "-Xverify", &tail)) {
      if (strcmp(tail, ":all") == 0 || strcmp(tail, "") == 0) {
        if (FLAG_SET_CMDLINE(bool, BytecodeVerificationLocal, true) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
        if (FLAG_SET_CMDLINE(bool, BytecodeVerificationRemote, true) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
      } else if (strcmp(tail, ":remote") == 0) {
        if (FLAG_SET_CMDLINE(bool, BytecodeVerificationLocal, false) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
        if (FLAG_SET_CMDLINE(bool, BytecodeVerificationRemote, true) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
      } else if (strcmp(tail, ":none") == 0) {
        if (FLAG_SET_CMDLINE(bool, BytecodeVerificationLocal, false) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
        if (FLAG_SET_CMDLINE(bool, BytecodeVerificationRemote, false) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
      } else if (is_bad_option(option, args->ignoreUnrecognized, "verification")) {
        return JNI_EINVAL;
      }
    // -Xdebug
    } else if (match_option(option, "-Xdebug")) {
      // note this flag has been used, then ignore
      set_xdebug_mode(true);
    // -Xnoagent
    } else if (match_option(option, "-Xnoagent")) {
      // For compatibility with classic. HotSpot refuses to load the old style agent.dll.
    } else if (match_option(option, "-Xloggc:", &tail)) {
      // Deprecated flag to redirect GC output to a file. -Xloggc:<filename>
      log_warning(gc)("-Xloggc is deprecated. Will use -Xlog:gc:%s instead.", tail);
      _gc_log_filename = os::strdup_check_oom(tail);
    } else if (match_option(option, "-Xlog", &tail)) {
      bool ret = false;
      if (strcmp(tail, ":help") == 0) {
        fileStream stream(defaultStream::output_stream());
        LogConfiguration::print_command_line_help(&stream);
        vm_exit(0);
      } else if (strcmp(tail, ":disable") == 0) {
        LogConfiguration::disable_logging();
        ret = true;
      } else if (*tail == '\0') {
        ret = LogConfiguration::parse_command_line_arguments();
        assert(ret, "-Xlog without arguments should never fail to parse");
      } else if (*tail == ':') {
        ret = LogConfiguration::parse_command_line_arguments(tail + 1);
      }
      if (ret == false) {
        jio_fprintf(defaultStream::error_stream(),
                    "Invalid -Xlog option '-Xlog%s', see error log for details.\n",
                    tail);
        return JNI_EINVAL;
      }
    // JNI hooks
    } else if (match_option(option, "-Xcheck", &tail)) {
      if (!strcmp(tail, ":jni")) {
#if !INCLUDE_JNI_CHECK
        warning("JNI CHECKING is not supported in this VM");
#else
        CheckJNICalls = true;
#endif // INCLUDE_JNI_CHECK
      } else if (is_bad_option(option, args->ignoreUnrecognized,
                                     "check")) {
        return JNI_EINVAL;
      }
    } else if (match_option(option, "vfprintf")) {
      _vfprintf_hook = CAST_TO_FN_PTR(vfprintf_hook_t, option->extraInfo);
    } else if (match_option(option, "exit")) {
      _exit_hook = CAST_TO_FN_PTR(exit_hook_t, option->extraInfo);
    } else if (match_option(option, "abort")) {
      _abort_hook = CAST_TO_FN_PTR(abort_hook_t, option->extraInfo);
    // Need to keep consistency of MaxTenuringThreshold and AlwaysTenure/NeverTenure;
    // and the last option wins.
    } else if (match_option(option, "-XX:+NeverTenure")) {
      if (FLAG_SET_CMDLINE(bool, NeverTenure, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(bool, AlwaysTenure, false) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(uintx, MaxTenuringThreshold, markOopDesc::max_age + 1) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    } else if (match_option(option, "-XX:+AlwaysTenure")) {
      if (FLAG_SET_CMDLINE(bool, NeverTenure, false) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(bool, AlwaysTenure, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(uintx, MaxTenuringThreshold, 0) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    } else if (match_option(option, "-XX:MaxTenuringThreshold=", &tail)) {
      uintx max_tenuring_thresh = 0;
      if (!parse_uintx(tail, &max_tenuring_thresh, 0)) {
        jio_fprintf(defaultStream::error_stream(),
                    "Improperly specified VM option \'MaxTenuringThreshold=%s\'\n", tail);
        return JNI_EINVAL;
      }

      if (FLAG_SET_CMDLINE(uintx, MaxTenuringThreshold, max_tenuring_thresh) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }

      if (MaxTenuringThreshold == 0) {
        if (FLAG_SET_CMDLINE(bool, NeverTenure, false) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
        if (FLAG_SET_CMDLINE(bool, AlwaysTenure, true) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
      } else {
        if (FLAG_SET_CMDLINE(bool, NeverTenure, false) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
        if (FLAG_SET_CMDLINE(bool, AlwaysTenure, false) != JVMFlag::SUCCESS) {
          return JNI_EINVAL;
        }
      }
    } else if (match_option(option, "-XX:+DisplayVMOutputToStderr")) {
      if (FLAG_SET_CMDLINE(bool, DisplayVMOutputToStdout, false) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(bool, DisplayVMOutputToStderr, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    } else if (match_option(option, "-XX:+DisplayVMOutputToStdout")) {
      if (FLAG_SET_CMDLINE(bool, DisplayVMOutputToStderr, false) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(bool, DisplayVMOutputToStdout, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    } else if (match_option(option, "-XX:+ErrorFileToStderr")) {
      if (FLAG_SET_CMDLINE(bool, ErrorFileToStdout, false) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(bool, ErrorFileToStderr, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    } else if (match_option(option, "-XX:+ErrorFileToStdout")) {
      if (FLAG_SET_CMDLINE(bool, ErrorFileToStderr, false) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(bool, ErrorFileToStdout, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
    } else if (match_option(option, "-XX:+ExtendedDTraceProbes")) {
#if defined(DTRACE_ENABLED)
      if (FLAG_SET_CMDLINE(bool, ExtendedDTraceProbes, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(bool, DTraceMethodProbes, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(bool, DTraceAllocProbes, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      if (FLAG_SET_CMDLINE(bool, DTraceMonitorProbes, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
#else // defined(DTRACE_ENABLED)
      jio_fprintf(defaultStream::error_stream(),
                  "ExtendedDTraceProbes flag is not applicable for this configuration\n");
      return JNI_EINVAL;
#endif // defined(DTRACE_ENABLED)
#ifdef ASSERT
    } else if (match_option(option, "-XX:+FullGCALot")) {
      if (FLAG_SET_CMDLINE(bool, FullGCALot, true) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
      // disable scavenge before parallel mark-compact
      if (FLAG_SET_CMDLINE(bool, ScavengeBeforeFullGC, false) != JVMFlag::SUCCESS) {
        return JNI_EINVAL;
      }
#endif
#if !INCLUDE_MANAGEMENT
    } else if (match_option(option, "-XX:+ManagementServer")) {
        jio_fprintf(defaultStream::error_stream(),
          "ManagementServer is not supported in this VM.\n");
        return JNI_ERR;
#endif // INCLUDE_MANAGEMENT
#if INCLUDE_JFR
    } else if (match_jfr_option(&option)) {
      return JNI_EINVAL;
#endif
    } else if (match_option(option, "-XX:", &tail)) { // -XX:xxxx
      // Skip -XX:Flags= and -XX:VMOptionsFile= since those cases have
      // already been handled
      if ((strncmp(tail, "Flags=", strlen("Flags=")) != 0) &&
          (strncmp(tail, "VMOptionsFile=", strlen("VMOptionsFile=")) != 0)) {
        if (!process_argument(tail, args->ignoreUnrecognized, origin)) {
          return JNI_EINVAL;
        }
      }
    // Unknown option
    } else if (is_bad_option(option, args->ignoreUnrecognized)) {
      return JNI_ERR;
    }
  }

  // PrintSharedArchiveAndExit will turn on
  //   -Xshare:on
  //   -Xlog:class+path=info
  if (PrintSharedArchiveAndExit) {
    if (FLAG_SET_CMDLINE(bool, UseSharedSpaces, true) != JVMFlag::SUCCESS) {
      return JNI_EINVAL;
    }
    if (FLAG_SET_CMDLINE(bool, RequireSharedSpaces, true) != JVMFlag::SUCCESS) {
      return JNI_EINVAL;
    }
    LogConfiguration::configure_stdout(LogLevel::Info, true, LOG_TAGS(class, path));
  }

  // Change the default value for flags  which have different default values
  // when working with older JDKs.
#ifdef LINUX
 if (JDK_Version::current().compare_major(6) <= 0 &&
      FLAG_IS_DEFAULT(UseLinuxPosixThreadCPUClocks)) {
    FLAG_SET_DEFAULT(UseLinuxPosixThreadCPUClocks, false);
  }
#endif // LINUX
  fix_appclasspath();
  return JNI_OK;
}

void Arguments::add_patch_mod_prefix(const char* module_name, const char* path, bool* patch_mod_javabase) {
  // For java.base check for duplicate --patch-module options being specified on the command line.
  // This check is only required for java.base, all other duplicate module specifications
  // will be checked during module system initialization.  The module system initialization
  // will throw an ExceptionInInitializerError if this situation occurs.
  if (strcmp(module_name, JAVA_BASE_NAME) == 0) {
    if (*patch_mod_javabase) {
      vm_exit_during_initialization("Cannot specify " JAVA_BASE_NAME " more than once to --patch-module");
    } else {
      *patch_mod_javabase = true;
    }
  }

  // Create GrowableArray lazily, only if --patch-module has been specified
  if (_patch_mod_prefix == NULL) {
    _patch_mod_prefix = new (ResourceObj::C_HEAP, mtArguments) GrowableArray<ModulePatchPath*>(10, true);
  }

  _patch_mod_prefix->push(new ModulePatchPath(module_name, path));
}

// Remove all empty paths from the app classpath (if IgnoreEmptyClassPaths is enabled)
//
// This is necessary because some apps like to specify classpath like -cp foo.jar:${XYZ}:bar.jar
// in their start-up scripts. If XYZ is empty, the classpath will look like "-cp foo.jar::bar.jar".
// Java treats such empty paths as if the user specified "-cp foo.jar:.:bar.jar". I.e., an empty
// path is treated as the current directory.
//
// This causes problems with CDS, which requires that all directories specified in the classpath
// must be empty. In most cases, applications do NOT want to load classes from the current
// directory anyway. Adding -XX:+IgnoreEmptyClassPaths will make these applications' start-up
// scripts compatible with CDS.
void Arguments::fix_appclasspath() {
  if (IgnoreEmptyClassPaths) {
    const char separator = *os::path_separator();
    const char* src = _java_class_path->value();

    // skip over all the leading empty paths
    while (*src == separator) {
      src ++;
    }

    char* copy = os::strdup_check_oom(src, mtArguments);

    // trim all trailing empty paths
    for (char* tail = copy + strlen(copy) - 1; tail >= copy && *tail == separator; tail--) {
      *tail = '\0';
    }

    char from[3] = {separator, separator, '\0'};
    char to  [2] = {separator, '\0'};
    while (StringUtils::replace_no_expand(copy, from, to) > 0) {
      // Keep replacing "::" -> ":" until we have no more "::" (non-windows)
      // Keep replacing ";;" -> ";" until we have no more ";;" (windows)
    }

    _java_class_path->set_writeable_value(copy);
    FreeHeap(copy); // a copy was made by set_value, so don't need this anymore
  }
}

jint Arguments::finalize_vm_init_args(bool patch_mod_javabase) {
  // check if the default lib/endorsed directory exists; if so, error
  char path[JVM_MAXPATHLEN];
  const char* fileSep = os::file_separator();
  jio_snprintf(path, JVM_MAXPATHLEN, "%s%slib%sendorsed", Arguments::get_java_home(), fileSep, fileSep);

  DIR* dir = os::opendir(path);
  if (dir != NULL) {
    jio_fprintf(defaultStream::output_stream(),
      "<JAVA_HOME>/lib/endorsed is not supported. Endorsed standards and standalone APIs\n"
      "in modular form will be supported via the concept of upgradeable modules.\n");
    os::closedir(dir);
    return JNI_ERR;
  }

  jio_snprintf(path, JVM_MAXPATHLEN, "%s%slib%sext", Arguments::get_java_home(), fileSep, fileSep);
  dir = os::opendir(path);
  if (dir != NULL) {
    jio_fprintf(defaultStream::output_stream(),
      "<JAVA_HOME>/lib/ext exists, extensions mechanism no longer supported; "
      "Use -classpath instead.\n.");
    os::closedir(dir);
    return JNI_ERR;
  }

  // This must be done after all arguments have been processed
  // and the container support has been initialized since AggressiveHeap
  // relies on the amount of total memory available.
  if (AggressiveHeap) {
    jint result = set_aggressive_heap_flags();
    if (result != JNI_OK) {
      return result;
    }
  }

  // This must be done after all arguments have been processed.
  // java_compiler() true means set to "NONE" or empty.
  if (java_compiler() && !xdebug_mode()) {
    // For backwards compatibility, we switch to interpreted mode if
    // -Djava.compiler="NONE" or "" is specified AND "-Xdebug" was
    // not specified.
    set_mode_flags(_int);
  }

  // CompileThresholdScaling == 0.0 is same as -Xint: Disable compilation (enable interpreter-only mode),
  // but like -Xint, leave compilation thresholds unaffected.
  // With tiered compilation disabled, setting CompileThreshold to 0 disables compilation as well.
  if ((CompileThresholdScaling == 0.0) || (!TieredCompilation && CompileThreshold == 0)) {
    set_mode_flags(_int);
  }

#ifdef ZERO
  // Zero always runs in interpreted mode
  set_mode_flags(_int);
#endif

  // eventually fix up InitialTenuringThreshold if only MaxTenuringThreshold is set
  if (FLAG_IS_DEFAULT(InitialTenuringThreshold) && (InitialTenuringThreshold > MaxTenuringThreshold)) {
    FLAG_SET_ERGO(uintx, InitialTenuringThreshold, MaxTenuringThreshold);
  }

#if !COMPILER2_OR_JVMCI
  // Don't degrade server performance for footprint
  if (FLAG_IS_DEFAULT(UseLargePages) &&
      MaxHeapSize < LargePageHeapSizeThreshold) {
    // No need for large granularity pages w/small heaps.
    // Note that large pages are enabled/disabled for both the
    // Java heap and the code cache.
    FLAG_SET_DEFAULT(UseLargePages, false);
  }

  UNSUPPORTED_OPTION(ProfileInterpreter);
  NOT_PRODUCT(UNSUPPORTED_OPTION(TraceProfileInterpreter));
#endif

#ifndef TIERED
  // Tiered compilation is undefined.
  UNSUPPORTED_OPTION(TieredCompilation);
#endif

  if (!check_vm_args_consistency()) {
    return JNI_ERR;
  }

#if INCLUDE_CDS
  if (DumpSharedSpaces) {
    // Disable biased locking now as it interferes with the clean up of
    // the archived Klasses and Java string objects (at dump time only).
    UseBiasedLocking = false;

    // Always verify non-system classes during CDS dump
    if (!BytecodeVerificationRemote) {
      BytecodeVerificationRemote = true;
      log_info(cds)("All non-system classes will be verified (-Xverify:remote) during CDS dump time.");
    }

    // Compilation is already disabled if the user specifies -Xshare:dump.
    // Disable compilation in case user specifies -XX:+DumpSharedSpaces instead of -Xshare:dump.
    set_mode_flags(_int);
  }
  if (UseSharedSpaces && patch_mod_javabase) {
    no_shared_spaces("CDS is disabled when " JAVA_BASE_NAME " module is patched.");
  }
  if (UseSharedSpaces && !DumpSharedSpaces && check_unsupported_cds_runtime_properties()) {
    FLAG_SET_DEFAULT(UseSharedSpaces, false);
  }
#endif

#ifndef CAN_SHOW_REGISTERS_ON_ASSERT
  UNSUPPORTED_OPTION(ShowRegistersOnAssert);
#endif // CAN_SHOW_REGISTERS_ON_ASSERT

  return JNI_OK;
}

// Helper class for controlling the lifetime of JavaVMInitArgs
// objects.  The contents of the JavaVMInitArgs are guaranteed to be
// deleted on the destruction of the ScopedVMInitArgs object.
class ScopedVMInitArgs : public StackObj {
 private:
  JavaVMInitArgs _args;
  char*          _container_name;
  bool           _is_set;
  char*          _vm_options_file_arg;

 public:
  ScopedVMInitArgs(const char *container_name) {
    _args.version = JNI_VERSION_1_2;
    _args.nOptions = 0;
    _args.options = NULL;
    _args.ignoreUnrecognized = false;
    _container_name = (char *)container_name;
    _is_set = false;
    _vm_options_file_arg = NULL;
  }

  // Populates the JavaVMInitArgs object represented by this
  // ScopedVMInitArgs object with the arguments in options.  The
  // allocated memory is deleted by the destructor.  If this method
  // returns anything other than JNI_OK, then this object is in a
  // partially constructed state, and should be abandoned.
  jint set_args(GrowableArray<JavaVMOption>* options) {
    _is_set = true;
    JavaVMOption* options_arr = NEW_C_HEAP_ARRAY_RETURN_NULL(
        JavaVMOption, options->length(), mtArguments);
    if (options_arr == NULL) {
      return JNI_ENOMEM;
    }
    _args.options = options_arr;

    for (int i = 0; i < options->length(); i++) {
      options_arr[i] = options->at(i);
      options_arr[i].optionString = os::strdup(options_arr[i].optionString);
      if (options_arr[i].optionString == NULL) {
        // Rely on the destructor to do cleanup.
        _args.nOptions = i;
        return JNI_ENOMEM;
      }
    }

    _args.nOptions = options->length();
    _args.ignoreUnrecognized = IgnoreUnrecognizedVMOptions;
    return JNI_OK;
  }

  JavaVMInitArgs* get()             { return &_args; }
  char* container_name()            { return _container_name; }
  bool  is_set()                    { return _is_set; }
  bool  found_vm_options_file_arg() { return _vm_options_file_arg != NULL; }
  char* vm_options_file_arg()       { return _vm_options_file_arg; }

  void set_vm_options_file_arg(const char *vm_options_file_arg) {
    if (_vm_options_file_arg != NULL) {
      os::free(_vm_options_file_arg);
    }
    _vm_options_file_arg = os::strdup_check_oom(vm_options_file_arg);
  }

  ~ScopedVMInitArgs() {
    if (_vm_options_file_arg != NULL) {
      os::free(_vm_options_file_arg);
    }
    if (_args.options == NULL) return;
    for (int i = 0; i < _args.nOptions; i++) {
      os::free(_args.options[i].optionString);
    }
    FREE_C_HEAP_ARRAY(JavaVMOption, _args.options);
  }

  // Insert options into this option list, to replace option at
  // vm_options_file_pos (-XX:VMOptionsFile)
  jint insert(const JavaVMInitArgs* args,
              const JavaVMInitArgs* args_to_insert,
              const int vm_options_file_pos) {
    assert(_args.options == NULL, "shouldn't be set yet");
    assert(args_to_insert->nOptions != 0, "there should be args to insert");
    assert(vm_options_file_pos != -1, "vm_options_file_pos should be set");

    int length = args->nOptions + args_to_insert->nOptions - 1;
    GrowableArray<JavaVMOption> *options = new (ResourceObj::C_HEAP, mtArguments)
              GrowableArray<JavaVMOption>(length, true);    // Construct new option array
    for (int i = 0; i < args->nOptions; i++) {
      if (i == vm_options_file_pos) {
        // insert the new options starting at the same place as the
        // -XX:VMOptionsFile option
        for (int j = 0; j < args_to_insert->nOptions; j++) {
          options->push(args_to_insert->options[j]);
        }
      } else {
        options->push(args->options[i]);
      }
    }
    // make into options array
    jint result = set_args(options);
    delete options;
    return result;
  }
};

jint Arguments::parse_java_options_environment_variable(ScopedVMInitArgs* args) {
  return parse_options_environment_variable("_JAVA_OPTIONS", args);
}

jint Arguments::parse_java_tool_options_environment_variable(ScopedVMInitArgs* args) {
  return parse_options_environment_variable("JAVA_TOOL_OPTIONS", args);
}

jint Arguments::parse_options_environment_variable(const char* name,
                                                   ScopedVMInitArgs* vm_args) {
  char *buffer = ::getenv(name);

  // Don't check this environment variable if user has special privileges
  // (e.g. unix su command).
  if (buffer == NULL || os::have_special_privileges()) {
    return JNI_OK;
  }

  if ((buffer = os::strdup(buffer)) == NULL) {
    return JNI_ENOMEM;
  }

  jio_fprintf(defaultStream::error_stream(),
              "Picked up %s: %s\n", name, buffer);

  int retcode = parse_options_buffer(name, buffer, strlen(buffer), vm_args);

  os::free(buffer);
  return retcode;
}

jint Arguments::parse_vm_options_file(const char* file_name, ScopedVMInitArgs* vm_args) {
  // read file into buffer
  int fd = ::open(file_name, O_RDONLY);
  if (fd < 0) {
    jio_fprintf(defaultStream::error_stream(),
                "Could not open options file '%s'\n",
                file_name);
    return JNI_ERR;
  }

  struct stat stbuf;
  int retcode = os::stat(file_name, &stbuf);
  if (retcode != 0) {
    jio_fprintf(defaultStream::error_stream(),
                "Could not stat options file '%s'\n",
                file_name);
    os::close(fd);
    return JNI_ERR;
  }

  if (stbuf.st_size == 0) {
    // tell caller there is no option data and that is ok
    os::close(fd);
    return JNI_OK;
  }

  // '+ 1' for NULL termination even with max bytes
  size_t bytes_alloc = stbuf.st_size + 1;

  char *buf = NEW_C_HEAP_ARRAY_RETURN_NULL(char, bytes_alloc, mtArguments);
  if (NULL == buf) {
    jio_fprintf(defaultStream::error_stream(),
                "Could not allocate read buffer for options file parse\n");
    os::close(fd);
    return JNI_ENOMEM;
  }

  memset(buf, 0, bytes_alloc);

  // Fill buffer
  // Use ::read() instead of os::read because os::read()
  // might do a thread state transition
  // and it is too early for that here

  ssize_t bytes_read = ::read(fd, (void *)buf, (unsigned)bytes_alloc);
  os::close(fd);
  if (bytes_read < 0) {
    FREE_C_HEAP_ARRAY(char, buf);
    jio_fprintf(defaultStream::error_stream(),
                "Could not read options file '%s'\n", file_name);
    return JNI_ERR;
  }

  if (bytes_read == 0) {
    // tell caller there is no option data and that is ok
    FREE_C_HEAP_ARRAY(char, buf);
    return JNI_OK;
  }

  retcode = parse_options_buffer(file_name, buf, bytes_read, vm_args);

  FREE_C_HEAP_ARRAY(char, buf);
  return retcode;
}

jint Arguments::parse_options_buffer(const char* name, char* buffer, const size_t buf_len, ScopedVMInitArgs* vm_args) {
  GrowableArray<JavaVMOption> *options = new (ResourceObj::C_HEAP, mtArguments) GrowableArray<JavaVMOption>(2, true);    // Construct option array

  // some pointers to help with parsing
  char *buffer_end = buffer + buf_len;
  char *opt_hd = buffer;
  char *wrt = buffer;
  char *rd = buffer;

  // parse all options
  while (rd < buffer_end) {
    // skip leading white space from the input string
    while (rd < buffer_end && isspace(*rd)) {
      rd++;
    }

    if (rd >= buffer_end) {
      break;
    }

    // Remember this is where we found the head of the token.
    opt_hd = wrt;

    // Tokens are strings of non white space characters separated
    // by one or more white spaces.
    while (rd < buffer_end && !isspace(*rd)) {
      if (*rd == '\'' || *rd == '"') {      // handle a quoted string
        int quote = *rd;                    // matching quote to look for
        rd++;                               // don't copy open quote
        while (rd < buffer_end && *rd != quote) {
                                            // include everything (even spaces)
                                            // up until the close quote
          *wrt++ = *rd++;                   // copy to option string
        }

        if (rd < buffer_end) {
          rd++;                             // don't copy close quote
        } else {
                                            // did not see closing quote
          jio_fprintf(defaultStream::error_stream(),
                      "Unmatched quote in %s\n", name);
          delete options;
          return JNI_ERR;
        }
      } else {
        *wrt++ = *rd++;                     // copy to option string
      }
    }

    // steal a white space character and set it to NULL
    *wrt++ = '\0';
    // We now have a complete token

    JavaVMOption option;
    option.optionString = opt_hd;
    option.extraInfo = NULL;

    options->append(option);                // Fill in option

    rd++;  // Advance to next character
  }

  // Fill out JavaVMInitArgs structure.
  jint status = vm_args->set_args(options);

  delete options;
  return status;
}

void Arguments::set_shared_spaces_flags() {
  if (DumpSharedSpaces) {
    if (FailOverToOldVerifier) {
      // Don't fall back to the old verifier on verification failure. If a
      // class fails verification with the split verifier, it might fail the
      // CDS runtime verifier constraint check. In that case, we don't want
      // to share the class. We only archive classes that pass the split verifier.
      FLAG_SET_DEFAULT(FailOverToOldVerifier, false);
    }

    if (RequireSharedSpaces) {
      warning("Cannot dump shared archive while using shared archive");
    }
    UseSharedSpaces = false;
#ifdef _LP64
    if (!UseCompressedOops || !UseCompressedClassPointers) {
      vm_exit_during_initialization(
        "Cannot dump shared archive when UseCompressedOops or UseCompressedClassPointers is off.", NULL);
    }
  } else {
    if (!UseCompressedOops || !UseCompressedClassPointers) {
      no_shared_spaces("UseCompressedOops and UseCompressedClassPointers must be on for UseSharedSpaces.");
    }
#endif
  }
}

// Sharing support
// Construct the path to the archive
static char* get_shared_archive_path() {
  char *shared_archive_path;
  if (SharedArchiveFile == NULL) {
    char jvm_path[JVM_MAXPATHLEN];
    os::jvm_path(jvm_path, sizeof(jvm_path));
    char *end = strrchr(jvm_path, *os::file_separator());
    if (end != NULL) *end = '\0';
    size_t jvm_path_len = strlen(jvm_path);
    size_t file_sep_len = strlen(os::file_separator());
    const size_t len = jvm_path_len + file_sep_len + 20;
    shared_archive_path = NEW_C_HEAP_ARRAY(char, len, mtArguments);
    if (shared_archive_path != NULL) {
      jio_snprintf(shared_archive_path, len, "%s%sclasses.jsa",
        jvm_path, os::file_separator());
    }
  } else {
    shared_archive_path = os::strdup_check_oom(SharedArchiveFile, mtArguments);
  }
  return shared_archive_path;
}

#ifndef PRODUCT
// Determine whether LogVMOutput should be implicitly turned on.
static bool use_vm_log() {
  if (LogCompilation || !FLAG_IS_DEFAULT(LogFile) ||
      PrintCompilation || PrintInlining || PrintDependencies || PrintNativeNMethods ||
      PrintDebugInfo || PrintRelocations || PrintNMethods || PrintExceptionHandlers ||
      PrintAssembly || TraceDeoptimization || TraceDependencies ||
      (VerifyDependencies && FLAG_IS_CMDLINE(VerifyDependencies))) {
    return true;
  }

#ifdef COMPILER1
  if (PrintC1Statistics) {
    return true;
  }
#endif // COMPILER1

#ifdef COMPILER2
  if (PrintOptoAssembly || PrintOptoStatistics) {
    return true;
  }
#endif // COMPILER2

  return false;
}

#endif // PRODUCT

bool Arguments::args_contains_vm_options_file_arg(const JavaVMInitArgs* args) {
  for (int index = 0; index < args->nOptions; index++) {
    const JavaVMOption* option = args->options + index;
    const char* tail;
    if (match_option(option, "-XX:VMOptionsFile=", &tail)) {
      return true;
    }
  }
  return false;
}

jint Arguments::insert_vm_options_file(const JavaVMInitArgs* args,
                                       const char* vm_options_file,
                                       const int vm_options_file_pos,
                                       ScopedVMInitArgs* vm_options_file_args,
                                       ScopedVMInitArgs* args_out) {
  jint code = parse_vm_options_file(vm_options_file, vm_options_file_args);
  if (code != JNI_OK) {
    return code;
  }

  if (vm_options_file_args->get()->nOptions < 1) {
    return JNI_OK;
  }

  if (args_contains_vm_options_file_arg(vm_options_file_args->get())) {
    jio_fprintf(defaultStream::error_stream(),
                "A VM options file may not refer to a VM options file. "
                "Specification of '-XX:VMOptionsFile=<file-name>' in the "
                "options file '%s' in options container '%s' is an error.\n",
                vm_options_file_args->vm_options_file_arg(),
                vm_options_file_args->container_name());
    return JNI_EINVAL;
  }

  return args_out->insert(args, vm_options_file_args->get(),
                          vm_options_file_pos);
}

// Expand -XX:VMOptionsFile found in args_in as needed.
// mod_args and args_out parameters may return values as needed.
jint Arguments::expand_vm_options_as_needed(const JavaVMInitArgs* args_in,
                                            ScopedVMInitArgs* mod_args,
                                            JavaVMInitArgs** args_out) {
  jint code = match_special_option_and_act(args_in, mod_args);
  if (code != JNI_OK) {
    return code;
  }

  if (mod_args->is_set()) {
    // args_in contains -XX:VMOptionsFile and mod_args contains the
    // original options from args_in along with the options expanded
    // from the VMOptionsFile. Return a short-hand to the caller.
    *args_out = mod_args->get();
  } else {
    *args_out = (JavaVMInitArgs *)args_in;  // no changes so use args_in
  }
  return JNI_OK;
}

jint Arguments::match_special_option_and_act(const JavaVMInitArgs* args,
                                             ScopedVMInitArgs* args_out) {
  // Remaining part of option string
  const char* tail;
  ScopedVMInitArgs vm_options_file_args(args_out->container_name());

  for (int index = 0; index < args->nOptions; index++) {
    const JavaVMOption* option = args->options + index;
    if (ArgumentsExt::process_options(option)) {
      continue;
    }
    if (match_option(option, "-XX:Flags=", &tail)) {
      Arguments::set_jvm_flags_file(tail);
      continue;
    }
    if (match_option(option, "-XX:VMOptionsFile=", &tail)) {
      if (vm_options_file_args.found_vm_options_file_arg()) {
        jio_fprintf(defaultStream::error_stream(),
                    "The option '%s' is already specified in the options "
                    "container '%s' so the specification of '%s' in the "
                    "same options container is an error.\n",
                    vm_options_file_args.vm_options_file_arg(),
                    vm_options_file_args.container_name(),
                    option->optionString);
        return JNI_EINVAL;
      }
      vm_options_file_args.set_vm_options_file_arg(option->optionString);
      // If there's a VMOptionsFile, parse that
      jint code = insert_vm_options_file(args, tail, index,
                                         &vm_options_file_args, args_out);
      if (code != JNI_OK) {
        return code;
      }
      args_out->set_vm_options_file_arg(vm_options_file_args.vm_options_file_arg());
      if (args_out->is_set()) {
        // The VMOptions file inserted some options so switch 'args'
        // to the new set of options, and continue processing which
        // preserves "last option wins" semantics.
        args = args_out->get();
        // The first option from the VMOptionsFile replaces the
        // current option.  So we back track to process the
        // replacement option.
        index--;
      }
      continue;
    }
    if (match_option(option, "-XX:+PrintVMOptions")) {
      PrintVMOptions = true;
      continue;
    }
    if (match_option(option, "-XX:-PrintVMOptions")) {
      PrintVMOptions = false;
      continue;
    }
    if (match_option(option, "-XX:+IgnoreUnrecognizedVMOptions")) {
      IgnoreUnrecognizedVMOptions = true;
      continue;
    }
    if (match_option(option, "-XX:-IgnoreUnrecognizedVMOptions")) {
      IgnoreUnrecognizedVMOptions = false;
      continue;
    }
    if (match_option(option, "-XX:+PrintFlagsInitial")) {
      JVMFlag::printFlags(tty, false);
      vm_exit(0);
    }
    if (match_option(option, "-XX:NativeMemoryTracking", &tail)) {
#if INCLUDE_NMT
      // The launcher did not setup nmt environment variable properly.
      if (!MemTracker::check_launcher_nmt_support(tail)) {
        warning("Native Memory Tracking did not setup properly, using wrong launcher?");
      }

      // Verify if nmt option is valid.
      if (MemTracker::verify_nmt_option()) {
        // Late initialization, still in single-threaded mode.
        if (MemTracker::tracking_level() >= NMT_summary) {
          MemTracker::init();
        }
      } else {
        vm_exit_during_initialization("Syntax error, expecting -XX:NativeMemoryTracking=[off|summary|detail]", NULL);
      }
      continue;
#else
      jio_fprintf(defaultStream::error_stream(),
        "Native Memory Tracking is not supported in this VM\n");
      return JNI_ERR;
#endif
    }

#ifndef PRODUCT
    if (match_option(option, "-XX:+PrintFlagsWithComments")) {
      JVMFlag::printFlags(tty, true);
      vm_exit(0);
    }
#endif
  }
  return JNI_OK;
}

static void print_options(const JavaVMInitArgs *args) {
  const char* tail;
  for (int index = 0; index < args->nOptions; index++) {
    const JavaVMOption *option = args->options + index;
    if (match_option(option, "-XX:", &tail)) {
      logOption(tail);
    }
  }
}

bool Arguments::handle_deprecated_print_gc_flags() {
  if (PrintGC) {
    log_warning(gc)("-XX:+PrintGC is deprecated. Will use -Xlog:gc instead.");
  }
  if (PrintGCDetails) {
    log_warning(gc)("-XX:+PrintGCDetails is deprecated. Will use -Xlog:gc* instead.");
  }

  if (_gc_log_filename != NULL) {
    // -Xloggc was used to specify a filename
    const char* gc_conf = PrintGCDetails ? "gc*" : "gc";

    LogTarget(Error, logging) target;
    LogStream errstream(target);
    return LogConfiguration::parse_log_arguments(_gc_log_filename, gc_conf, NULL, NULL, &errstream);
  } else if (PrintGC || PrintGCDetails) {
    LogConfiguration::configure_stdout(LogLevel::Info, !PrintGCDetails, LOG_TAGS(gc));
  }
  return true;
}

void Arguments::handle_extra_cms_flags(const char* msg) {
  SpecialFlag flag;
  const char *flag_name = "UseConcMarkSweepGC";
  if (lookup_special_flag(flag_name, flag)) {
    handle_aliases_and_deprecation(flag_name, /* print warning */ true);
    warning("%s", msg);
  }
}

// Parse entry point called from JNI_CreateJavaVM

jint Arguments::parse(const JavaVMInitArgs* initial_cmd_args) {
  assert(verify_special_jvm_flags(), "deprecated and obsolete flag table inconsistent");

  // Initialize ranges, constraints and writeables
  JVMFlagRangeList::init();
  JVMFlagConstraintList::init();
  JVMFlagWriteableList::init();

  // If flag "-XX:Flags=flags-file" is used it will be the first option to be processed.
  const char* hotspotrc = ".hotspotrc";
  bool settings_file_specified = false;
  bool needs_hotspotrc_warning = false;
  ScopedVMInitArgs initial_vm_options_args("");
  ScopedVMInitArgs initial_java_tool_options_args("env_var='JAVA_TOOL_OPTIONS'");
  ScopedVMInitArgs initial_java_options_args("env_var='_JAVA_OPTIONS'");

  // Pointers to current working set of containers
  JavaVMInitArgs* cur_cmd_args;
  JavaVMInitArgs* cur_vm_options_args;
  JavaVMInitArgs* cur_java_options_args;
  JavaVMInitArgs* cur_java_tool_options_args;

  // Containers for modified/expanded options
  ScopedVMInitArgs mod_cmd_args("cmd_line_args");
  ScopedVMInitArgs mod_vm_options_args("vm_options_args");
  ScopedVMInitArgs mod_java_tool_options_args("env_var='JAVA_TOOL_OPTIONS'");
  ScopedVMInitArgs mod_java_options_args("env_var='_JAVA_OPTIONS'");


  jint code =
      parse_java_tool_options_environment_variable(&initial_java_tool_options_args);
  if (code != JNI_OK) {
    return code;
  }

  code = parse_java_options_environment_variable(&initial_java_options_args);
  if (code != JNI_OK) {
    return code;
  }

  // Parse the options in the /java.base/jdk/internal/vm/options resource, if present
  char *vmoptions = ClassLoader::lookup_vm_options();
  if (vmoptions != NULL) {
    code = parse_options_buffer("vm options resource", vmoptions, strlen(vmoptions), &initial_vm_options_args);
    FREE_C_HEAP_ARRAY(char, vmoptions);
    if (code != JNI_OK) {
      return code;
    }
  }

  code = expand_vm_options_as_needed(initial_java_tool_options_args.get(),
                                     &mod_java_tool_options_args,
                                     &cur_java_tool_options_args);
  if (code != JNI_OK) {
    return code;
  }

  code = expand_vm_options_as_needed(initial_cmd_args,
                                     &mod_cmd_args,
                                     &cur_cmd_args);
  if (code != JNI_OK) {
    return code;
  }

  code = expand_vm_options_as_needed(initial_java_options_args.get(),
                                     &mod_java_options_args,
                                     &cur_java_options_args);
  if (code != JNI_OK) {
    return code;
  }

  code = expand_vm_options_as_needed(initial_vm_options_args.get(),
                                     &mod_vm_options_args,
                                     &cur_vm_options_args);
  if (code != JNI_OK) {
    return code;
  }

  const char* flags_file = Arguments::get_jvm_flags_file();
  settings_file_specified = (flags_file != NULL);

  if (IgnoreUnrecognizedVMOptions) {
    cur_cmd_args->ignoreUnrecognized = true;
    cur_java_tool_options_args->ignoreUnrecognized = true;
    cur_java_options_args->ignoreUnrecognized = true;
  }

  // Parse specified settings file
  if (settings_file_specified) {
    if (!process_settings_file(flags_file, true,
                               cur_cmd_args->ignoreUnrecognized)) {
      return JNI_EINVAL;
    }
  } else {
#ifdef ASSERT
    // Parse default .hotspotrc settings file
    if (!process_settings_file(".hotspotrc", false,
                               cur_cmd_args->ignoreUnrecognized)) {
      return JNI_EINVAL;
    }
#else
    struct stat buf;
    if (os::stat(hotspotrc, &buf) == 0) {
      needs_hotspotrc_warning = true;
    }
#endif
  }

  if (PrintVMOptions) {
    print_options(cur_java_tool_options_args);
    print_options(cur_cmd_args);
    print_options(cur_java_options_args);
  }

  // Parse JavaVMInitArgs structure passed in, as well as JAVA_TOOL_OPTIONS and _JAVA_OPTIONS
  jint result = parse_vm_init_args(cur_vm_options_args,
                                   cur_java_tool_options_args,
                                   cur_java_options_args,
                                   cur_cmd_args);

  if (result != JNI_OK) {
    return result;
  }

  // Call get_shared_archive_path() here, after possible SharedArchiveFile option got parsed.
  SharedArchivePath = get_shared_archive_path();
  if (SharedArchivePath == NULL) {
    return JNI_ENOMEM;
  }

  // Set up VerifySharedSpaces
  if (FLAG_IS_DEFAULT(VerifySharedSpaces) && SharedArchiveFile != NULL) {
    VerifySharedSpaces = true;
  }

  // Delay warning until here so that we've had a chance to process
  // the -XX:-PrintWarnings flag
  if (needs_hotspotrc_warning) {
    warning("%s file is present but has been ignored.  "
            "Run with -XX:Flags=%s to load the file.",
            hotspotrc, hotspotrc);
  }

  if (needs_module_property_warning) {
    warning("Ignoring system property options whose names match the '-Djdk.module.*'."
            " names that are reserved for internal use.");
  }

#if defined(_ALLBSD_SOURCE) || defined(AIX)  // UseLargePages is not yet supported on BSD and AIX.
  UNSUPPORTED_OPTION(UseLargePages);
#endif

#if defined(AIX)
  UNSUPPORTED_OPTION_NULL(AllocateHeapAt);
#endif

  ArgumentsExt::report_unsupported_options();

#ifndef PRODUCT
  if (TraceBytecodesAt != 0) {
    TraceBytecodes = true;
  }
  if (CountCompiledCalls) {
    if (UseCounterDecay) {
      warning("UseCounterDecay disabled because CountCalls is set");
      UseCounterDecay = false;
    }
  }
#endif // PRODUCT

  if (ScavengeRootsInCode == 0) {
    if (!FLAG_IS_DEFAULT(ScavengeRootsInCode)) {
      warning("Forcing ScavengeRootsInCode non-zero");
    }
    ScavengeRootsInCode = 1;
  }

  if (!handle_deprecated_print_gc_flags()) {
    return JNI_EINVAL;
  }

  // Set object alignment values.
  set_object_alignment();

#if !INCLUDE_CDS
  if (DumpSharedSpaces || RequireSharedSpaces) {
    jio_fprintf(defaultStream::error_stream(),
      "Shared spaces are not supported in this VM\n");
    return JNI_ERR;
  }
  if ((UseSharedSpaces && FLAG_IS_CMDLINE(UseSharedSpaces)) ||
      log_is_enabled(Info, cds)) {
    warning("Shared spaces are not supported in this VM");
    FLAG_SET_DEFAULT(UseSharedSpaces, false);
    LogConfiguration::configure_stdout(LogLevel::Off, true, LOG_TAGS(cds));
  }
  no_shared_spaces("CDS Disabled");
#endif // INCLUDE_CDS

  return JNI_OK;
}

jint Arguments::apply_ergo() {
  // Set flags based on ergonomics.
  jint result = set_ergonomics_flags();
  if (result != JNI_OK) return result;

  // Set heap size based on available physical memory
  set_heap_size();

  GCConfig::arguments()->initialize();

  set_shared_spaces_flags();

  // Initialize Metaspace flags and alignments
  Metaspace::ergo_initialize();

  // Set compiler flags after GC is selected and GC specific
  // flags (LoopStripMiningIter) are set.
  CompilerConfig::ergo_initialize();

  // Set bytecode rewriting flags
  set_bytecode_flags();

  // Set flags if Aggressive optimization flags (-XX:+AggressiveOpts) enabled
  jint code = set_aggressive_opts_flags();
  if (code != JNI_OK) {
    return code;
  }

  // Turn off biased locking for locking debug mode flags,
  // which are subtly different from each other but neither works with
  // biased locking
  if (UseHeavyMonitors
#ifdef COMPILER1
      || !UseFastLocking
#endif // COMPILER1
#if INCLUDE_JVMCI
      || !JVMCIUseFastLocking
#endif
    ) {
    if (!FLAG_IS_DEFAULT(UseBiasedLocking) && UseBiasedLocking) {
      // flag set to true on command line; warn the user that they
      // can't enable biased locking here
      warning("Biased Locking is not supported with locking debug flags"
              "; ignoring UseBiasedLocking flag." );
    }
    UseBiasedLocking = false;
  }

#ifdef CC_INTERP
  // Clear flags not supported on zero.
  FLAG_SET_DEFAULT(ProfileInterpreter, false);
  FLAG_SET_DEFAULT(UseBiasedLocking, false);
  LP64_ONLY(FLAG_SET_DEFAULT(UseCompressedOops, false));
  LP64_ONLY(FLAG_SET_DEFAULT(UseCompressedClassPointers, false));
#endif // CC_INTERP

  if (PrintAssembly && FLAG_IS_DEFAULT(DebugNonSafepoints)) {
    warning("PrintAssembly is enabled; turning on DebugNonSafepoints to gain additional output");
    DebugNonSafepoints = true;
  }

  if (FLAG_IS_CMDLINE(CompressedClassSpaceSize) && !UseCompressedClassPointers) {
    warning("Setting CompressedClassSpaceSize has no effect when compressed class pointers are not used");
  }

#ifndef PRODUCT
  if (!LogVMOutput && FLAG_IS_DEFAULT(LogVMOutput)) {
    if (use_vm_log()) {
      LogVMOutput = true;
    }
  }
#endif // PRODUCT

  if (PrintCommandLineFlags) {
    JVMFlag::printSetFlags(tty);
  }

  // Apply CPU specific policy for the BiasedLocking
  if (UseBiasedLocking) {
    if (!VM_Version::use_biased_locking() &&
        !(FLAG_IS_CMDLINE(UseBiasedLocking))) {
      UseBiasedLocking = false;
    }
  }
#ifdef COMPILER2
  if (!UseBiasedLocking || EmitSync != 0) {
    UseOptoBiasInlining = false;
  }
#endif

#if defined(IA32)
  // Only server compiler can optimize safepoints well enough.
  if (!is_server_compilation_mode_vm()) {
    FLAG_SET_ERGO_IF_DEFAULT(bool, ThreadLocalHandshakes, false);
  }
#endif

  // ThreadLocalHandshakesConstraintFunc handles the constraints.
  if (FLAG_IS_DEFAULT(ThreadLocalHandshakes) || !SafepointMechanism::supports_thread_local_poll()) {
    log_debug(ergo)("ThreadLocalHandshakes %s", ThreadLocalHandshakes ? "enabled." : "disabled.");
  } else {
    log_info(ergo)("ThreadLocalHandshakes %s", ThreadLocalHandshakes ? "enabled." : "disabled.");
  }

  return JNI_OK;
}

jint Arguments::adjust_after_os() {
  if (UseNUMA) {
    if (!FLAG_IS_DEFAULT(AllocateHeapAt)) {
      FLAG_SET_ERGO(bool, UseNUMA, false);
    } else if (UseParallelGC || UseParallelOldGC) {
      if (FLAG_IS_DEFAULT(MinHeapDeltaBytes)) {
         FLAG_SET_DEFAULT(MinHeapDeltaBytes, 64*M);
      }
    }
    // UseNUMAInterleaving is set to ON for all collectors and
    // platforms when UseNUMA is set to ON. NUMA-aware collectors
    // such as the parallel collector for Linux and Solaris will
    // interleave old gen and survivor spaces on top of NUMA
    // allocation policy for the eden space.
    // Non NUMA-aware collectors such as CMS, G1 and Serial-GC on
    // all platforms and ParallelGC on Windows will interleave all
    // of the heap spaces across NUMA nodes.
    if (FLAG_IS_DEFAULT(UseNUMAInterleaving)) {
      FLAG_SET_ERGO(bool, UseNUMAInterleaving, true);
    }
  }
  return JNI_OK;
}

int Arguments::PropertyList_count(SystemProperty* pl) {
  int count = 0;
  while(pl != NULL) {
    count++;
    pl = pl->next();
  }
  return count;
}

// Return the number of readable properties.
int Arguments::PropertyList_readable_count(SystemProperty* pl) {
  int count = 0;
  while(pl != NULL) {
    if (pl->is_readable()) {
      count++;
    }
    pl = pl->next();
  }
  return count;
}

const char* Arguments::PropertyList_get_value(SystemProperty *pl, const char* key) {
  assert(key != NULL, "just checking");
  SystemProperty* prop;
  for (prop = pl; prop != NULL; prop = prop->next()) {
    if (strcmp(key, prop->key()) == 0) return prop->value();
  }
  return NULL;
}

// Return the value of the requested property provided that it is a readable property.
const char* Arguments::PropertyList_get_readable_value(SystemProperty *pl, const char* key) {
  assert(key != NULL, "just checking");
  SystemProperty* prop;
  // Return the property value if the keys match and the property is not internal or
  // it's the special internal property "jdk.boot.class.path.append".
  for (prop = pl; prop != NULL; prop = prop->next()) {
    if (strcmp(key, prop->key()) == 0) {
      if (!prop->internal()) {
        return prop->value();
      } else if (strcmp(key, "jdk.boot.class.path.append") == 0) {
        return prop->value();
      } else {
        // Property is internal and not jdk.boot.class.path.append so return NULL.
        return NULL;
      }
    }
  }
  return NULL;
}

const char* Arguments::PropertyList_get_key_at(SystemProperty *pl, int index) {
  int count = 0;
  const char* ret_val = NULL;

  while(pl != NULL) {
    if(count >= index) {
      ret_val = pl->key();
      break;
    }
    count++;
    pl = pl->next();
  }

  return ret_val;
}

char* Arguments::PropertyList_get_value_at(SystemProperty* pl, int index) {
  int count = 0;
  char* ret_val = NULL;

  while(pl != NULL) {
    if(count >= index) {
      ret_val = pl->value();
      break;
    }
    count++;
    pl = pl->next();
  }

  return ret_val;
}

void Arguments::PropertyList_add(SystemProperty** plist, SystemProperty *new_p) {
  SystemProperty* p = *plist;
  if (p == NULL) {
    *plist = new_p;
  } else {
    while (p->next() != NULL) {
      p = p->next();
    }
    p->set_next(new_p);
  }
}

void Arguments::PropertyList_add(SystemProperty** plist, const char* k, const char* v,
                                 bool writeable, bool internal) {
  if (plist == NULL)
    return;

  SystemProperty* new_p = new SystemProperty(k, v, writeable, internal);
  PropertyList_add(plist, new_p);
}

void Arguments::PropertyList_add(SystemProperty *element) {
  PropertyList_add(&_system_properties, element);
}

// This add maintains unique property key in the list.
void Arguments::PropertyList_unique_add(SystemProperty** plist, const char* k, const char* v,
                                        PropertyAppendable append, PropertyWriteable writeable,
                                        PropertyInternal internal) {
  if (plist == NULL)
    return;

  // If property key exists and is writeable, then update with new value.
  // Trying to update a non-writeable property is silently ignored.
  SystemProperty* prop;
  for (prop = *plist; prop != NULL; prop = prop->next()) {
    if (strcmp(k, prop->key()) == 0) {
      if (append == AppendProperty) {
        prop->append_writeable_value(v);
      } else {
        prop->set_writeable_value(v);
      }
      return;
    }
  }

  PropertyList_add(plist, k, v, writeable == WriteableProperty, internal == InternalProperty);
}

// Copies src into buf, replacing "%%" with "%" and "%p" with pid
// Returns true if all of the source pointed by src has been copied over to
// the destination buffer pointed by buf. Otherwise, returns false.
// Notes:
// 1. If the length (buflen) of the destination buffer excluding the
// NULL terminator character is not long enough for holding the expanded
// pid characters, it also returns false instead of returning the partially
// expanded one.
// 2. The passed in "buflen" should be large enough to hold the null terminator.
bool Arguments::copy_expand_pid(const char* src, size_t srclen,
                                char* buf, size_t buflen) {
  const char* p = src;
  char* b = buf;
  const char* src_end = &src[srclen];
  char* buf_end = &buf[buflen - 1];

  while (p < src_end && b < buf_end) {
    if (*p == '%') {
      switch (*(++p)) {
      case '%':         // "%%" ==> "%"
        *b++ = *p++;
        break;
      case 'p':  {       //  "%p" ==> current process id
        // buf_end points to the character before the last character so
        // that we could write '\0' to the end of the buffer.
        size_t buf_sz = buf_end - b + 1;
        int ret = jio_snprintf(b, buf_sz, "%d", os::current_process_id());

        // if jio_snprintf fails or the buffer is not long enough to hold
        // the expanded pid, returns false.
        if (ret < 0 || ret >= (int)buf_sz) {
          return false;
        } else {
          b += ret;
          assert(*b == '\0', "fail in copy_expand_pid");
          if (p == src_end && b == buf_end + 1) {
            // reach the end of the buffer.
            return true;
          }
        }
        p++;
        break;
      }
      default :
        *b++ = '%';
      }
    } else {
      *b++ = *p++;
    }
  }
  *b = '\0';
  return (p == src_end); // return false if not all of the source was copied
}
