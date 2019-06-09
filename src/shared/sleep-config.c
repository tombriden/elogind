/* SPDX-License-Identifier: LGPL-2.1+ */
/***
  Copyright © 2018 Dell Inc.
***/

//#include <errno.h>
//#include <fcntl.h>
#include <linux/fs.h>
//#include <stdbool.h>
//#include <stddef.h>
//#include <stdio.h>
//#include <string.h>
#include <sys/ioctl.h>
//#include <sys/stat.h>
//#include <sys/types.h>
//#include <syslog.h>
//#include <unistd.h>

#include "alloc-util.h"
//#include "conf-parser.h"
//#include "def.h"
//#include "env-util.h"
//#include "errno-util.h"
#include "fd-util.h"
#include "fileio.h"
//#include "log.h"
//#include "macro.h"
#include "parse-util.h"
#include "path-util.h"
#include "sleep-config.h"
#include "string-util.h"
#include "strv.h"
//#include "time-util.h"

#if 0 /// UNNEEDED by elogind
int parse_sleep_config(SleepConfig **ret_sleep_config) {
        _cleanup_(free_sleep_configp) SleepConfig *sc;
        int allow_suspend = -1, allow_hibernate = -1,
            allow_s2h = -1, allow_hybrid_sleep = -1;

        sc = new0(SleepConfig, 1);
        if (!sc)
                return log_oom();

        const ConfigTableItem items[] = {
                { "Sleep", "AllowSuspend",              config_parse_tristate, 0, &allow_suspend },
                { "Sleep", "AllowHibernation",          config_parse_tristate, 0, &allow_hibernate },
                { "Sleep", "AllowSuspendThenHibernate", config_parse_tristate, 0, &allow_s2h },
                { "Sleep", "AllowHybridSleep",          config_parse_tristate, 0, &allow_hybrid_sleep },

                { "Sleep", "SuspendMode",               config_parse_strv, 0, &sc->suspend_modes  },
                { "Sleep", "SuspendState",              config_parse_strv, 0, &sc->suspend_states },
                { "Sleep", "HibernateMode",             config_parse_strv, 0, &sc->hibernate_modes  },
                { "Sleep", "HibernateState",            config_parse_strv, 0, &sc->hibernate_states },
                { "Sleep", "HybridSleepMode",           config_parse_strv, 0, &sc->hybrid_modes  },
                { "Sleep", "HybridSleepState",          config_parse_strv, 0, &sc->hybrid_states },

                { "Sleep", "HibernateDelaySec",         config_parse_sec,  0, &sc->hibernate_delay_sec},
                {}
        };

        (void) config_parse_many_nulstr(PKGSYSCONFDIR "/sleep.conf",
                                        CONF_PATHS_NULSTR("systemd/sleep.conf.d"),
                                        "Sleep\0", config_item_table_lookup, items,
                                        CONFIG_PARSE_WARN, NULL);

        /* use default values unless set */
        sc->allow_suspend = allow_suspend != 0;
        sc->allow_hibernate = allow_hibernate != 0;
        sc->allow_hybrid_sleep = allow_hybrid_sleep >= 0 ? allow_hybrid_sleep
                : (allow_suspend != 0 && allow_hibernate != 0);
        sc->allow_s2h = allow_s2h >= 0 ? allow_s2h
                : (allow_suspend != 0 && allow_hibernate != 0);

        if (!sc->suspend_states)
                sc->suspend_states = strv_new("mem", "standby", "freeze");
        if (!sc->hibernate_modes)
                sc->hibernate_modes = strv_new("platform", "shutdown");
        if (!sc->hibernate_states)
                sc->hibernate_states = strv_new("disk");
        if (!sc->hybrid_modes)
                sc->hybrid_modes = strv_new("suspend", "platform", "shutdown");
        if (!sc->hybrid_states)
                sc->hybrid_states = strv_new("disk");
        if (sc->hibernate_delay_sec == 0)
                sc->hibernate_delay_sec = 180 * USEC_PER_MINUTE;

        /* ensure values set for all required fields */
        if (!sc->suspend_states || !sc->hibernate_modes
            || !sc->hibernate_states || !sc->hybrid_modes || !sc->hybrid_states)
                return log_oom();

        *ret_sleep_config = TAKE_PTR(sc);

        return 0;
}
#endif // 0

#if 1 /// Only available in this file for elogind
static
#endif // 1
int can_sleep_state(char **types) {
        char **type;
        int r;
        _cleanup_free_ char *p = NULL;

        if (strv_isempty(types))
                return true;

        /* If /sys is read-only we cannot sleep */
        if (access("/sys/power/state", W_OK) < 0)
                return false;

        r = read_one_line_file("/sys/power/state", &p);
        if (r < 0)
                return false;

        STRV_FOREACH(type, types) {
                const char *word, *state;
                size_t l, k;

                k = strlen(*type);
                FOREACH_WORD_SEPARATOR(word, l, p, WHITESPACE, state)
                        if (l == k && memcmp(word, *type, l) == 0)
                                return true;
        }

        return false;
}

#if 1 /// Only available in this file for elogind
static
#endif // 1
int can_sleep_disk(char **types) {
        char **type;
        int r;
        _cleanup_free_ char *p = NULL;

        if (strv_isempty(types))
                return true;

        /* If /sys is read-only we cannot sleep */
        if (access("/sys/power/disk", W_OK) < 0) {
                log_debug_errno(errno, "/sys/power/disk is not writable: %m");
                return false;
        }

        r = read_one_line_file("/sys/power/disk", &p);
        if (r < 0) {
                log_debug_errno(r, "Couldn't read /sys/power/disk: %m");
                return false;
        }

        STRV_FOREACH(type, types) {
                const char *word, *state;
                size_t l, k;

                k = strlen(*type);
                FOREACH_WORD_SEPARATOR(word, l, p, WHITESPACE, state) {
                        if (l == k && memcmp(word, *type, l) == 0)
                                return true;

                        if (l == k + 2 &&
                            word[0] == '[' &&
                            memcmp(word + 1, *type, l - 2) == 0 &&
                            word[l-1] == ']')
                                return true;
                }
        }

        return false;
}

#define HIBERNATION_SWAP_THRESHOLD 0.98

int find_hibernate_location(char **device, char **type, size_t *size, size_t *used) {
        _cleanup_fclose_ FILE *f;
        unsigned i;

        f = fopen("/proc/swaps", "re");
        if (!f) {
                log_full(errno == ENOENT ? LOG_DEBUG : LOG_WARNING,
                         "Failed to retrieve open /proc/swaps: %m");
                return negative_errno();
        }

        (void) fscanf(f, "%*s %*s %*s %*s %*s\n");

        // TODO: sort swaps in priority order rather than using first successful option
        for (i = 1;; i++) {
                _cleanup_free_ char *dev_field = NULL, *type_field = NULL;
                size_t size_field, used_field;
                int k;

                k = fscanf(f,
                           "%ms "   /* device/file */
                           "%ms "   /* type of swap */
                           "%zu "   /* swap size */
                           "%zu "   /* used */
                           "%*i\n", /* priority */
                           &dev_field, &type_field, &size_field, &used_field);
                if (k == EOF)
                        break;
                if (k != 4) {
                        log_warning("Failed to parse /proc/swaps:%u", i);
                        continue;
                }

                if (streq(type_field, "file")) {

                        if (endswith(dev_field, "\\040(deleted)")) {
                                log_warning("Ignoring deleted swap file '%s'.", dev_field);
                                continue;
                        }

                } else if (streq(type_field, "partition")) {
                        const char *fn;

                        fn = path_startswith(dev_field, "/dev/");
                        if (fn && startswith(fn, "zram")) {
                                log_debug("Ignoring compressed RAM swap device '%s'.", dev_field);
                                continue;
                        }
                }

                if (device)
                        *device = TAKE_PTR(dev_field);
                if (type)
                        *type = TAKE_PTR(type_field);
                if (size)
                        *size = size_field;
                if (used)
                        *used = used_field;
                return 0;
        }

        return log_debug_errno(SYNTHETIC_ERRNO(ENOSYS),
                               "No swap partitions were found.");
}

static bool enough_swap_for_hibernation(void) {
        _cleanup_free_ char *active = NULL;
        unsigned long long act = 0;
        size_t size = 0, used = 0;
        int r;

#if 0 /// elogind does not allow any bypass, we are never init!
        if (getenv_bool("SYSTEMD_BYPASS_HIBERNATION_MEMORY_CHECK") > 0)
                return true;
#endif // 0

        r = find_hibernate_location(NULL, NULL, &size, &used);
        if (r < 0)
                return false;

        r = get_proc_field("/proc/meminfo", "Active(anon)", WHITESPACE, &active);
        if (r < 0) {
                log_debug_errno(r, "Failed to retrieve Active(anon) from /proc/meminfo: %m");
                return false;
        }

        r = safe_atollu(active, &act);
        if (r < 0) {
                log_debug_errno(r, "Failed to parse Active(anon) from /proc/meminfo: %s: %m", active);
                return false;
        }

        r = act <= (size - used) * HIBERNATION_SWAP_THRESHOLD;
        log_debug("%s swap for hibernation, Active(anon)=%llu kB, size=%zu kB, used=%zu kB, threshold=%.2g%%",
                  r ? "Enough" : "Not enough", act, size, used, 100*HIBERNATION_SWAP_THRESHOLD);

        return r;
}

int read_fiemap(int fd, struct fiemap **ret) {
        _cleanup_free_ struct fiemap *fiemap = NULL, *result_fiemap = NULL;
        struct stat statinfo;
        uint32_t result_extents = 0;
        uint64_t fiemap_start = 0, fiemap_length;
        const size_t n_extra = DIV_ROUND_UP(sizeof(struct fiemap), sizeof(struct fiemap_extent));
        size_t fiemap_allocated = n_extra, result_fiemap_allocated = n_extra;

        if (fstat(fd, &statinfo) < 0)
                return log_debug_errno(errno, "Cannot determine file size: %m");
        if (!S_ISREG(statinfo.st_mode))
                return -ENOTTY;
        fiemap_length = statinfo.st_size;

        /* Zero this out in case we run on a file with no extents */
        fiemap = calloc(n_extra, sizeof(struct fiemap_extent));
        if (!fiemap)
                return -ENOMEM;

        result_fiemap = malloc_multiply(n_extra, sizeof(struct fiemap_extent));
        if (!result_fiemap)
                return -ENOMEM;

        /*  XFS filesystem has incorrect implementation of fiemap ioctl and
         *  returns extents for only one block-group at a time, so we need
         *  to handle it manually, starting the next fiemap call from the end
         *  of the last extent
         */
        while (fiemap_start < fiemap_length) {
                *fiemap = (struct fiemap) {
                        .fm_start = fiemap_start,
                        .fm_length = fiemap_length,
                        .fm_flags = FIEMAP_FLAG_SYNC,
                };

                /* Find out how many extents there are */
                if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0)
                        return log_debug_errno(errno, "Failed to read extents: %m");

                /* Nothing to process */
                if (fiemap->fm_mapped_extents == 0)
                        break;

                /* Resize fiemap to allow us to read in the extents, result fiemap has to hold all
                 * the extents for the whole file. Add space for the initial struct fiemap. */
                if (!greedy_realloc0((void**) &fiemap, &fiemap_allocated,
                                     n_extra + fiemap->fm_mapped_extents, sizeof(struct fiemap_extent)))
                        return -ENOMEM;

                fiemap->fm_extent_count = fiemap->fm_mapped_extents;
                fiemap->fm_mapped_extents = 0;

                if (ioctl(fd, FS_IOC_FIEMAP, fiemap) < 0)
                        return log_debug_errno(errno, "Failed to read extents: %m");

                /* Resize result_fiemap to allow us to copy in the extents */
                if (!greedy_realloc((void**) &result_fiemap, &result_fiemap_allocated,
                                    n_extra + result_extents + fiemap->fm_mapped_extents, sizeof(struct fiemap_extent)))
                        return -ENOMEM;

                memcpy(result_fiemap->fm_extents + result_extents,
                       fiemap->fm_extents,
                       sizeof(struct fiemap_extent) * fiemap->fm_mapped_extents);

                result_extents += fiemap->fm_mapped_extents;

                /* Highly unlikely that it is zero */
                if (_likely_(fiemap->fm_mapped_extents > 0)) {
                        uint32_t i = fiemap->fm_mapped_extents - 1;

                        fiemap_start = fiemap->fm_extents[i].fe_logical +
                                       fiemap->fm_extents[i].fe_length;

                        if (fiemap->fm_extents[i].fe_flags & FIEMAP_EXTENT_LAST)
                                break;
                }
        }

        memcpy(result_fiemap, fiemap, sizeof(struct fiemap));
        result_fiemap->fm_mapped_extents = result_extents;
        *ret = TAKE_PTR(result_fiemap);
        return 0;
}

#if 0 /// elogind has to ask the manager for some stuff
#else
static int can_sleep_internal(Manager *m, const char *verb, bool check_allowed);
#endif // 0

#if 0 /// elogind has to ask the manager for some stuff
static int can_sleep_internal(const char *verb, bool check_allowed, const SleepConfig *sleep_config);

static bool can_s2h(const SleepConfig *sleep_config) {
#else
static bool can_s2h(Manager *m) {
#endif // 0
        const char *p;
        int r;

        if (!clock_supported(CLOCK_BOOTTIME_ALARM)) {
                log_full(errno == ENOENT ? LOG_DEBUG : LOG_WARNING,
                         "CLOCK_BOOTTIME_ALARM is not supported");
                return false;
        }

        FOREACH_STRING(p, "suspend", "hibernate") {
#if 0 /// elogind must transport a pointer to its managers instance
                r = can_sleep_internal(p, false, sleep_config);
#else
                r = can_sleep_internal(m, p, false);
#endif // 0
                if (IN_SET(r, 0, -ENOSPC, -EADV)) {
                        log_debug("Unable to %s system.", p);
                        return false;
                }
                if (r < 0)
                        return log_debug_errno(r, "Failed to check if %s is possible: %m", p);
        }

        return true;
}


#if 0 /// elogind has to ask the manager for some stuff
static int can_sleep_internal(const char *verb, bool check_allowed, const SleepConfig *sleep_config) {
#else
static int can_sleep_internal(Manager *m, const char *verb, bool check_allowed) {
        assert(m);
#endif // 0
        bool allow;
#if 0 /// elogind has this parsed already
        char **modes = NULL, **states = NULL;
        int r;
#else
        char **modes  = streq(verb, "hibernate")    ? m->hibernate_mode    :
                        streq(verb, "hybrid-sleep") ? m->hybrid_sleep_mode :
                                                      m->suspend_mode;
        char **states = streq(verb, "hibernate")    ? m->hibernate_state    :
                        streq(verb, "hybrid-sleep") ? m->hybrid_sleep_state :
                                                      m->suspend_state;
#endif // 0

        assert(STR_IN_SET(verb, "suspend", "hibernate", "hybrid-sleep", "suspend-then-hibernate"));

#if 0 /// already parsed by elogind config
        r = sleep_settings(verb, sleep_config, &allow, &modes, &states);
        if (r < 0)
                return false;
#else
        allow = false;
        if ( (streq(verb, "suspend")                && m->allow_suspend)                || 
             (streq(verb, "hibernate")              && m->allow_hibernation)            ||
             (streq(verb, "suspend-then-hibernate") && m->allow_suspend_then_hibernate) ||
             (streq(verb, "hybrid-sleep")           && m->allow_hybrid_sleep) )
                allow = true;
#endif // 0

        if (check_allowed && !allow) {
#if 0 /// be a bit more verbose in elogind
                log_debug("Sleep mode \"%s\" is disabled by configuration.", verb);
#else
                log_info("Sleep mode \"%s\" is disabled by configuration.", verb);
                log_debug("allow_suspend               : %d", m->allow_suspend);
                log_debug("allow_hibernation           : %d", m->allow_hibernation);
                log_debug("allow_hybrid_sleep          : %d", m->allow_hybrid_sleep);
                log_debug("allow_suspend_then_hibernate: %d", m->allow_suspend_then_hibernate);
#endif // 0
                return false;
        }

        if (streq(verb, "suspend-then-hibernate"))
#if 0 /// elogind can ask its manager.
                return can_s2h(sleep_config);
#else
                return can_s2h(m);
#endif // 0

        if (!can_sleep_state(states) || !can_sleep_disk(modes))
                return false;

        if (streq(verb, "suspend"))
                return true;

        if (!enough_swap_for_hibernation())
                return -ENOSPC;

        return true;
}

#if 0 /// elogind has to ask the manager for some stuff
int can_sleep(const char *verb) {
        _cleanup_(free_sleep_configp) SleepConfig *sleep_config = NULL;
        int r;

        r = parse_sleep_config(&sleep_config);
        if (r < 0)
                return r;

        return can_sleep_internal(verb, true, sleep_config);
}

int sleep_settings(const char *verb, const SleepConfig *sleep_config, bool *ret_allow, char ***ret_modes, char ***ret_states) {

        assert(verb);
        assert(sleep_config);
        assert(STR_IN_SET(verb, "suspend", "hibernate", "hybrid-sleep", "suspend-then-hibernate"));

        if (streq(verb, "suspend")) {
                *ret_allow = sleep_config->allow_suspend;
                *ret_modes = sleep_config->suspend_modes;
                *ret_states = sleep_config->suspend_states;
        } else if (streq(verb, "hibernate")) {
                *ret_allow = sleep_config->allow_hibernate;
                *ret_modes = sleep_config->hibernate_modes;
                *ret_states = sleep_config->hibernate_states;
        } else if (streq(verb, "hybrid-sleep")) {
                *ret_allow = sleep_config->allow_hybrid_sleep;
                *ret_modes = sleep_config->hybrid_modes;
                *ret_states = sleep_config->hybrid_states;
        } else if (streq(verb, "suspend-then-hibernate")) {
                *ret_allow = sleep_config->allow_s2h;
                *ret_modes = *ret_states = NULL;
        }

        /* suspend modes empty by default */
        if ((!ret_modes && !streq(verb, "suspend")) || !ret_states)
                return log_error_errno(SYNTHETIC_ERRNO(EINVAL), "No modes or states set for %s; Check sleep.conf", verb);

        return 0;
}
#else
int can_sleep(Manager *m, const char *verb) {
        return can_sleep_internal(m, verb, true);

void free_sleep_config(SleepConfig *sc) {
        if (!sc)
                return;

        strv_free(sc->suspend_modes);
        strv_free(sc->suspend_states);

        strv_free(sc->hibernate_modes);
        strv_free(sc->hibernate_states);

        strv_free(sc->hybrid_modes);
        strv_free(sc->hybrid_states);

        free(sc);
}
#endif // 0
