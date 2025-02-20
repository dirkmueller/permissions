#ifndef CHKSTAT_H
#define CHKSTAT_H

// local headers
#include "utility.h"

// POSIX / Linux
#include <sys/types.h>

// third party
#include <tclap/CmdLine.h>

// C++
#include <map>
#include <set>
#include <string>
#include <vector>

/**
 * \brief
 *  Represents a single permissiosn profile entry
 **/
struct ProfileEntry
{
    std::string file;
    std::string owner;
    std::string group;
    mode_t mode = 0;
    FileCapabilities caps;

    bool hasCaps() const { return caps.valid(); }

    //! returns whether this profile entry contains a setuid or setgid bit
    bool hasSetXID() const
    {
        return (this->mode & (S_ISUID | S_ISGID)) != 0;
    }
};

/**
 * \brief
 *  Scratch data used while processing an individual ProfileEntry in
 *  processEntries()
 **/
struct EntryContext
{
    //! the resolved user-id corresponding to the active ProfileEntry
    uid_t uid = (uid_t)-1;
    //! the resolved group-id corresponding to the active ProfileEntry
    gid_t gid = (gid_t)-1;
    //! the path of the current file to check below a potential m_root_path
    std::string subpath;
    //! a path for safely opening the target file (typically in /proc/self/fd/...)
    std::string fd_path;
    //! the actual capabilities found on on the file
    FileCapabilities caps;
    //! the actual file status info found on the file
    FileStatus status;
    //! indicates whether actual file permissions need to be fixed
    bool need_fix_perms = false;
    //! indicates whether actual file capabilities need to be fixed
    bool need_fix_caps = false;
    //! indicates whether actual file user:group ownership needs to be fixed
    bool need_fix_ownership = false;
    //! safeOpen() flags this when it detects an insecure ownership
    //! constellation in the file's path.
    bool traversed_insecure = false;
    //! an O_PATH file descriptor for the target file which is set in
    //! safeOpen()
    FileDesc fd;

    //! based on the given \c entry sets need_fix_* members as required
    void check(const ProfileEntry &entry)
    {
        need_fix_perms = status.getModeBits() != entry.mode;
        need_fix_ownership = !status.matchesOwnership(this->uid, this->gid);
        need_fix_caps = entry.caps != this->caps;
    }

    bool needsFixing() const
    {
        return need_fix_perms || need_fix_caps || need_fix_ownership;
    }

    bool needFixPerms() const { return need_fix_perms; }
    bool needFixCaps() const { return need_fix_caps; }
    bool needFixOwnership() const { return need_fix_ownership; }

    bool traversedInsecure() const { return traversed_insecure; }
};

//! enum to differentiate different /proc availibility situations
enum class ProcMountState
{
    //! status was not investigated yet
    UNKNOWN,
    AVAIL,
    UNAVAIL,
};

/**
 * \brief
 *     Main application class for Chkstat
 **/
class Chkstat
{
public: // functions

    Chkstat(int argc, const char **argv);

    int run();

protected: // functions

    /**
     * \brief
     *     Validates command line arguments on syntactical and logical level
     **/
    bool validateArguments();

    /**
     * \brief
     *      Process the already validated command line arguments
     **/
    bool processArguments();

    bool needToCheck(const std::string &path) const
    {
        if (m_files_to_check.empty())
            // all paths should be checked
            return true;

        // otherwise only if the path is explicitly listed
        return m_files_to_check.find(path) != m_files_to_check.end();
    }

    std::string getUsrRoot() const
    {
        return m_config_root_path.getValue() + "/usr/share/permissions";
    }

    std::string getEtcRoot() const
    {
        return m_config_root_path.getValue() + "/etc";
    }

    bool parseSysconfig();

    bool checkFsCapsSupport() const;

    /**
     * \brief
     *      Adds the given profile (suffix) to the list of profiles to be
     *      processed
     **/
    void addProfile(const std::string &name);

    /**
     * \brief
     *      Collects all configured profiles
     *  \details
     *      Collects all profiles configured in m_profiles from /usr and /etc
     *      system directories and stores their paths in m_profile_paths.
     **/
    void collectProfilePaths();

    /**
     * \brief
     *      Collects configured per-package profiles from the given directory
     **/
    void collectPackageProfilePaths(const std::string &dir);

    /**
     * \brief
     *      Parses the given profile file and stores the according entries in
     *      m_profile_entries
     **/
    bool parseProfile(const std::string &path);

    /**
     * \brief
     *      Parses extra "+capabilities" lines in permission profiles
     * \param[in] line
     *      The input line from a profile file that starts with "+"
     * \param[input] active_paths
     *      The paths that the capabiliy line applies to. It is expected that
     *      these paths already have entries in m_profile_entries.
     * \return
     *      Whether the line could successfully be parsed
     **/
    bool parseCapabilityLine(const std::string &line, const std::vector<std::string> &active_paths);

    /**
     * \brief
     *      Adds a ProfileEntry to m_profile_entries for the given set of
     *      parameters
     **/
    ProfileEntry&
    addProfileEntry(const std::string &file, const std::string &owner, const std::string &group, mode_t mode);

    /**
     * \brief
     *      This function contains the actual profile entry traversal
     *      algorithm that carries out required file system operations
     **/
    int processEntries();

    /**
     * \brief
     *      Resolves the textual user:group in \c entry and stores the result
     *      in \c ctx
     **/
    bool resolveEntryOwnership(const ProfileEntry &entry, EntryContext &ctx);

    //! tests whether /proc is available in a caching manner
    bool checkHaveProc() const;

    //! prints an introductory text describing the active configuration
    void printHeader();

    //! outputs the difference that will (or should) be performed to arrived
    //! at the ProfileEntry configuration
    void printEntryDifferences(const ProfileEntry &entry, const EntryContext &ctx) const;

    //! performs checks whether it is safe to adjust the actual file given the
    //! collected information
    bool isSafeToApply(const ProfileEntry &entry, const EntryContext &ctx) const;

    //! actually perform the necessary changes for the actual file to arrive
    //! at the configuration given in \c entry.
    bool applyChanges(const ProfileEntry &entry, const EntryContext &ctx) const;

    /**
     * \brief
     *      Gets the currently set capabilities from ctx.fd_path and stores
     *      them in \c ctx.caps
     * \details
     *      \c entry is potentially modified if capabilities can't be applied.
     *      The return value indicates if an operational error occured but it
     *      doesn't indicate whether a capability valou could be assigned.
     *      Check ctx.caps.valid() for this.
     **/
    bool getCapabilities(ProfileEntry &entry, EntryContext &ctx);

    /**
     * \brief
     *      Safely open the target file taking symlinks and insecure
     *      constellations into account
     * \details
     *      On success the file descriptor will be stored in ctx.fd.
     * \return
     *      An indication whether the file could be successfully opened
     **/
    bool safeOpen(EntryContext &ctx);

    /**
     * \brief
     *      Resolves the file system path for the given file descriptor via
     *      /proc/self/fd
     **/
    std::string getPathFromProc(const FileDesc &fd) const;

    /**
     * \brief
     *      Parses the variables.conf file and fills m_variable_expansions
     **/
    void loadVariableExpansions();

    /**
     * \brief
     *      Expand possible variables in profile path specifications
     * \details
     *      Profile paths can contain %{variable} syntax that will expand to
     *      one or more alternative values. This function performs this
     *      expansion and returns the individual paths the profile entry
     *      corresponds to. It is possible that only a single entry results
     *      from the expansion or that no expansion is necessary at all, in
     *      which case only a single entry will be returned in \c paths.
     *  \return
     *      In case any unrecoverable parsing error is encountered \c false is
     *      returned and the profile entry should be ignored entirely.
     **/
    bool expandProfilePaths(const std::string &path, std::vector<std::string> &expansions);

protected: // data

    const int m_argc = 0;
    const char **m_argv = nullptr;

    TCLAP::CmdLine m_parser;

    TCLAP::SwitchArg m_system_mode;
    TCLAP::SwitchArg m_force_fscaps;
    TCLAP::SwitchArg m_disable_fscaps;
    SwitchArgRW m_apply_changes;
    TCLAP::SwitchArg m_only_warn;
    SwitchArgRW m_no_header;
    TCLAP::SwitchArg m_verbose;
    TCLAP::SwitchArg m_print_variables;

    // NOTE: previously chkstat allowed multiple specifications of value
    // switches like --level and --root but actually only used the last
    // occurence on the command line. In theory this is a backward
    // compatiblity break, but it's also kind of a bug.

    TCLAP::MultiArg<std::string> m_examine_paths;
    TCLAP::ValueArg<std::string> m_force_level_list;
    TCLAP::MultiArg<std::string> m_file_lists;

    SaneValueArg<std::string> m_root_path;
    //! alternate config root directory relative to which config files are
    //! looked up
    SaneValueArg<std::string> m_config_root_path;

    //! positional input arguments: either the files to check for --system
    //! mode or the profiles to parse for non-system mode.
    TCLAP::UnlabeledMultiArg<std::string> m_input_args;

    //! optional explicit set of files to check
    std::set<std::string> m_files_to_check;
    //! whether to touch file based capabilities. influenced by command line
    //! parameters and sysconfig configuration file
    bool m_use_fscaps = true;

    //! the default, predefined profile names shipped with permissions
    static constexpr const char * const PREDEFINED_PROFILES[] = {"easy", "secure", "paranoid"};

    //! permission profile names in the order they should be applied
    std::vector<std::string> m_profiles;

    //! permission profile paths in the order they should be applied
    std::vector<std::string> m_profile_paths;

    //! a mapping of file paths to ProfileEntry, denotes the entry to apply
    //! for each path
    std::map<std::string, ProfileEntry> m_profile_entries;

    /**
     * \brief
     *   A collection of the basenames of packages that have already been
     *   processed by collectPackageProfilePaths()
     **/
    std::set<std::string> m_package_profiles_seen;

    //! collected variable expansion mappings from the variables.conf file
    std::map<std::string, std::vector<std::string>> m_variable_expansions;

    //! the effective user ID we're running as
    const uid_t m_euid;
    //! the effective group ID we're running as
    const gid_t m_egid;

    mutable ProcMountState m_proc_mount_avail = ProcMountState::UNKNOWN;
};

#endif // inc. guard

// vim: et ts=4 sts=4 sw=4 :
