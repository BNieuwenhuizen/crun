/*
 * crun - OCI runtime written in C
 *
 * Copyright (C) 2017, 2018, 2019 Giuseppe Scrivano <giuseppe@scrivano.org>
 * crun is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * crun is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with crun.  If not, see <http://www.gnu.org/licenses/>.
 */

#define _GNU_SOURCE

#include <config.h>
#include "cgroup.h"
#include "cgroup-internal.h"
#include "cgroup-systemd.h"
#include "cgroup-setup.h"
#include "cgroup-utils.h"
#include "cgroup-cgroupfs.h"
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/vfs.h>
#include <inttypes.h>
#include <time.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <libgen.h>

static char *
make_cgroup_path (const char *path, const char *id)
{
  const char *cgroup_path = path;
  char *ret;

  if (cgroup_path == NULL)
    xasprintf (&ret, "/%s", id);
  else if (cgroup_path[0] == '/')
    ret = xstrdup (cgroup_path);
  else
    xasprintf (&ret, "/%s", cgroup_path);

  return ret;
}

static int
libcrun_precreate_cgroup_cgroupfs (struct libcrun_cgroup_args *args, int *dirfd, libcrun_error_t *err)
{
  cleanup_free char *sub_path = make_cgroup_path (args->cgroup_path, args->id);
  cleanup_free char *cgroup_path = NULL;
  int ret;

  /* No need to check the mode since this feature is supported only on cgroup v2, and
     libcrun_cgroup_preenter already performs this check.  */

  *dirfd = -1;

  ret = append_paths (&cgroup_path, err, CGROUP_ROOT, sub_path, NULL);
  if (UNLIKELY (ret < 0))
    return ret;

  ret = crun_ensure_directory (cgroup_path, 0755, true, err);
  if (UNLIKELY (ret < 0))
    {
      libcrun_error_release (err);
      return 0;
    }

  ret = enable_controllers (sub_path, err);
  if (UNLIKELY (ret < 0))
    return ret;

  *dirfd = open (cgroup_path, O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY | O_RDONLY);
  if (UNLIKELY (*dirfd < 0))
    return crun_make_error (err, errno, "open `%s`", cgroup_path);

  return 0;
}

static int
libcrun_cgroup_enter_cgroupfs (struct libcrun_cgroup_args *args, struct libcrun_cgroup_status *out, libcrun_error_t *err)
{
  pid_t pid = args->pid;
  int cgroup_mode;

  /* The cgroup was already joined, nothing more left to do.  */
  if (args->joined)
    return 0;

  cgroup_mode = libcrun_get_cgroup_mode (err);
  if (UNLIKELY (cgroup_mode < 0))
    return cgroup_mode;

  out->path = make_cgroup_path (args->cgroup_path, args->id);

  if (cgroup_mode == CGROUP_MODE_UNIFIED)
    {
      int ret;

      ret = enable_controllers (out->path, err);
      if (UNLIKELY (ret < 0))
        return ret;
    }

  return enter_cgroup (cgroup_mode, pid, 0, out->path, true, err);
}

static int
libcrun_destroy_cgroup_cgroupfs (struct libcrun_cgroup_status *cgroup_status,
                                 libcrun_error_t *err)
{
  int mode;
  int ret;

  mode = libcrun_get_cgroup_mode (err);
  if (UNLIKELY (mode < 0))
    return mode;

  ret = cgroup_killall_path (cgroup_status->path, SIGKILL, err);
  if (UNLIKELY (ret < 0))
    crun_error_release (err);

  return destroy_cgroup_path (cgroup_status->path, mode, err);
}

struct libcrun_cgroup_manager cgroup_manager_cgroupfs = {
  .precreate_cgroup = libcrun_precreate_cgroup_cgroupfs,
  .create_cgroup = libcrun_cgroup_enter_cgroupfs,
  .destroy_cgroup = libcrun_destroy_cgroup_cgroupfs,
};
