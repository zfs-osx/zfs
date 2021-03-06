/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.
 * Copyright (c) 2016 Jorgen Lundman <lundman@lundman.net>, based on nfs.c
 *                         by Gunnar Beutner
 *
 */

#include <time.h>
#include <stdlib.h>
#include <stdio.h>
#include <strings.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <libzfs.h>
#include <libshare.h>
#include <ctype.h>
#include <sys/socket.h>
#include "libshare_impl.h"
#include "smb.h"

static boolean_t smb_available(void);

static sa_fstype_t *smb_fstype;

#define	SMB_NAME_MAX		255

#define	SHARING_CMD_PATH		"/usr/sbin/sharing"

typedef struct smb_share_s {
	char name[SMB_NAME_MAX];	/* Share name */
	char path[PATH_MAX];		/* Share path */
	boolean_t guest_ok;		    /* boolean */

	boolean_t afpshared;	    /* AFP sharing on? */

	struct smb_share_s *next;
} smb_share_t;

smb_share_t *smb_shares = NULL;

/*
 * Parse out a "value" part of a "line" of input. By skipping white space.
 * If line ends up being empty, read the next line, skipping white spare.
 * strdup() value before returning.
 */
static int get_attribute(const char *attr, char *line, char **value, FILE *file)
{
	char *r = line;
	char line2[512];

	if (strncasecmp(attr, line, strlen(attr))) return 0;

	r += strlen(attr);

	//fprintf(stderr, "ZFS: matched '%s' in '%s'\r\n", attr, line);

	while(isspace(*r)) r++; // Skip whitespace

	// Nothing left? Read next line
	if (!*r) {
		if (!fgets(line2, sizeof(line2), file)) return 0;
		// Eat newlines
		if ((r = strchr(line2, '\r'))) *r = 0;
		if ((r = strchr(line2, '\n'))) *r = 0;
		// Parse new input
		r = line2;
		while(isspace(*r)) r++; // Skip whitespace
	}

	// Did we get something?
	if (*r) {
		*value = strdup(r);
		return 1;
	}
	return 0;
}

static int spawn_with_pipe(const char *path, char *argv[], int flags)
{
	int fd[2];
	pid_t pid;

	if( socketpair(AF_UNIX, SOCK_STREAM, 0, fd) != 0 ) return -1;

	pid = vfork();

	// Child
	if (pid == 0) {
		close(fd[0]);
		dup2(fd[1], STDIN_FILENO);
		dup2(fd[1], STDOUT_FILENO);
		if (flags) dup2(fd[1], STDERR_FILENO);
		(void) execvp(path, argv);
		_exit(-1);
	}
	// Parent and error
	close(fd[1]);
	if (pid == -1) {
		close(fd[0]);
		return -1;
	}
	return fd[0];
}




/*
 * Retrieve the list of SMB shares. We execute "dscl . -readall /SharePoints"
 * which gets us shares in the format:
 * dsAttrTypeNative:directory_path: /Volumes/BOOM/zfstest
 * dsAttrTypeNative:smb_name: zfstest
 * dsAttrTypeNative:smb_shared: 1
 * dsAttrTypeNative:smb_guestaccess: 1
 *
 * Note that long lines can be continued on the next line, with a leading space:
 * dsAttrTypeNative:smb_name:
 *  lundman's Public Folder
 *
 * We don't use "sharing -l" as its output format is "peculiar".
 *
 * This is a temporary implementation that should be replaced with
 * direct DirectoryService API calls.
 *
 */
static int
smb_retrieve_shares(void)
{
	char line[512];
	char *path = NULL, *shared = NULL, *name = NULL, *afpshared = NULL;
	char *guest = NULL, *r;
	smb_share_t *shares, *new_shares = NULL;
	int fd;
	FILE *file = NULL;
	char *argv[8] = {
		"/usr/bin/dscl",
		".",
		"-readall",
		"/SharePoints"
	};

	fd = spawn_with_pipe(argv[0], argv, 0);

	if (fd < 0)
		return (SA_SYSTEM_ERR);

	file = fdopen(fd, "r");
	if (!file) {
		close(fd);
		return (SA_SYSTEM_ERR);
	}

	while(fgets(line, sizeof(line), file)) {

		if ((r = strchr(line, '\r'))) *r = 0;
		if ((r = strchr(line, '\n'))) *r = 0;

		if (get_attribute("dsAttrTypeNative:smb_name:", line, &name, file) ||
			get_attribute("dsAttrTypeNative:directory_path:", line, &path, file) ||
			get_attribute("dsAttrTypeNative:smb_guestaccess:", line, &guest, file) ||
			get_attribute("dsAttrTypeNative:smb_shared:", line, &shared, file) ||
			get_attribute("dsAttrTypeNative:afp_shared:", line, &afpshared, file)) {

			// If we have all desired attributes, create a new share,
			// AND it is currently shared (not just listed)
			if (name && path && guest && shared && afpshared &&
				atoi(shared) != 0) {

				shares = (smb_share_t *)
						malloc(sizeof (smb_share_t));

				if (shares) {
					strlcpy(shares->name, name,
							sizeof (shares->name));
					strlcpy(shares->path, path,
							sizeof (shares->path));
					shares->guest_ok = atoi(guest);

					shares->afpshared = atoi(afpshared);

#ifdef DEBUG
					fprintf(stderr, "ZFS: smbshare '%s' mount '%s'\r\n",
							name, path);
#endif

					shares->next = new_shares;
					new_shares = shares;
				} // shares malloc

				// Make it free all variables
				strlcpy(line, "-", sizeof(line));

			} // if all

		} // if got_attribute

		if (!strncmp("-", line, sizeof(line))) {
			if (name)   {	free(name); 	name  = NULL; }
			if (path)   {	free(path); 	path  = NULL; }
			if (guest)  {	free(guest);	guest = NULL; }
			if (shared) {	free(shared);	shared = NULL; }
			if (afpshared) {free(afpshared);afpshared = NULL; }
		} // if "-"
	} // while fgets

	fclose(file);
	close(fd);

	if (name)   {	free(name); 	name  = NULL; }
	if (path)   {	free(path); 	path  = NULL; }
	if (guest)  {	free(guest);	guest = NULL; }
	if (shared) {	free(shared);	shared = NULL; }
	if (afpshared) {free(afpshared);afpshared = NULL; }

	/*
	 * The ZOL implementation here just leaks the previous list in
	 * "smb_shares" each time this is called, and it is called a lot.
	 * We really should iterate through and release nodes. Alternatively
	 * only update if we have not run before, and have a way to force
	 * a refresh after enabling/disabling a share.
	 */
	smb_shares = new_shares;

	return (SA_OK);
}

/*
 * Used internally by smb_enable_share to enable sharing for a single host.
 */
static int
smb_enable_share_one(const char *sharename, const char *sharepath)
{
	char *argv[10];
	int rc;
	smb_share_t *shares = smb_shares;
	int afpshared = 0;

	/* Loop through shares and check if our share is also smbshared */
	while (shares != NULL) {
		if (strcmp(sharepath, shares->path) == 0) {
			afpshared = shares->afpshared;
			break;
		}
		shares = shares->next;
	}

	/*
	 * CMD: sharing -a /mountpoint -s 001 -g 001
	 * Where -s 001 specified sharing smb, not ftp nor afp.
	 *   and -g 001 specifies to enable guest on smb.
	 * Note that the OS X 10.11 man-page incorrectly claims 010 for smb
	 */
	if (afpshared) {

		argv[0] = SHARING_CMD_PATH;
		argv[1] = (char *)"-e";
		argv[2] = (char *)sharename;
		argv[3] = (char *)"-s";
		argv[4] = (char *)"101";
		argv[5] = (char *)"-g";
		argv[6] = (char *)"101";
		argv[7] = NULL;

	} else {

		argv[0] = SHARING_CMD_PATH;
		argv[1] = (char *)"-a";
		argv[2] = (char *)sharepath;
		argv[3] = (char *)"-s";
		argv[4] = (char *)"001";
		argv[5] = (char *)"-g";
		argv[6] = (char *)"001";
		argv[7] = NULL;
	}

#ifdef DEBUG
	fprintf(stderr, "ZFS: enabling share '%s' at '%s'\r\n",
			sharename, sharepath);
#endif

	rc = libzfs_run_process(argv[0], argv, 0);
	if (rc < 0)
		return (SA_SYSTEM_ERR);

	/* Reload the share file */
	(void) smb_retrieve_shares();

	return (SA_OK);
}

/*
 * Enables SMB sharing for the specified share.
 */
static int
smb_enable_share(sa_share_impl_t impl_share)
{
	char *shareopts;

	if (!smb_available())
		return (SA_SYSTEM_ERR);

	shareopts = FSINFO(impl_share, smb_fstype)->shareopts;
	if (shareopts == NULL) /* on/off */
		return (SA_SYSTEM_ERR);

	if (strcmp(shareopts, "off") == 0)
		return (SA_OK);

	/* Magic: Enable (i.e., 'create new') share */
	return (smb_enable_share_one(impl_share->dataset,
	    impl_share->sharepath));
}

/*
 * Used internally by smb_disable_share to disable sharing for a single host.
 */
static int
smb_disable_share_one(const char *sharename, int afpshared)
{
	int rc;
	char *argv[8];

	// If AFP shared as well, we need to just remove SMB.
	if (afpshared) {

		argv[0] = SHARING_CMD_PATH;
		argv[1] = (char *)"-e";
		argv[2] = (char *)sharename;
		argv[3] = (char *)"-s";
		argv[4] = (char *)"100";  // SMB off, AFP on.
		argv[5] = (char *)"-g";
		argv[6] = (char *)"100";  // SMB off, AFP on.
		argv[7] = NULL;

	} else {  // Not SMB shared, just remove share

		/* CMD: sharing -r name */
		argv[0] = SHARING_CMD_PATH;
		argv[1] = (char *)"-r";
		argv[2] = (char *)sharename;
		argv[3] = NULL;
	}

#ifdef DEBUG
	fprintf(stderr, "ZFS: disabling share '%s' \r\n",
			sharename);
#endif

	rc = libzfs_run_process(argv[0], argv, 0);
	if (rc < 0)
		return (SA_SYSTEM_ERR);
	else
		return (SA_OK);
}

/*
 * Disables SMB sharing for the specified share.
 */
static int
smb_disable_share(sa_share_impl_t impl_share)
{
	smb_share_t *shares = smb_shares;

	if (!smb_available()) {
		/*
		 * The share can't possibly be active, so nothing
		 * needs to be done to disable it.
		 */
		return (SA_OK);
	}

	while (shares != NULL) {
		if (strcmp(impl_share->sharepath, shares->path) == 0)
			return (smb_disable_share_one(shares->name, shares->afpshared));

		shares = shares->next;
	}

	return (SA_OK);
}

/*
 * Checks whether the specified SMB share options are syntactically correct.
 */
static int
smb_validate_shareopts(const char *shareopts)
{
	/* TODO: Accept 'name' and sec/acl (?) */
	if ((strcmp(shareopts, "off") == 0) || (strcmp(shareopts, "on") == 0))
		return (SA_OK);

	return (SA_SYNTAX_ERR);
}

/*
 * Checks whether a share is currently active. Called from libzfs_mount
 */
boolean_t smb_is_mountpoint_active(const char *mountpoint)
{
	smb_retrieve_shares();

	while (smb_shares != NULL) {
		if (strcmp(mountpoint, smb_shares->path) == 0)
			return (B_TRUE);

		smb_shares = smb_shares->next;
	}

	return (B_FALSE);
}

static boolean_t
smb_is_share_active(sa_share_impl_t impl_share)
{
	return smb_is_mountpoint_active(impl_share->sharepath);
}



/*
 * Called to update a share's options. A share's options might be out of
 * date if the share was loaded from disk and the "sharesmb" dataset
 * property has changed in the meantime. This function also takes care
 * of re-enabling the share if necessary.
 */
static int
smb_update_shareopts(sa_share_impl_t impl_share, const char *resource,
    const char *shareopts)
{
	char *shareopts_dup;
	boolean_t needs_reshare = B_FALSE;
	char *old_shareopts;

	if (!impl_share)
		return (SA_SYSTEM_ERR);

	FSINFO(impl_share, smb_fstype)->active =
	    smb_is_share_active(impl_share);

	old_shareopts = FSINFO(impl_share, smb_fstype)->shareopts;

	if (FSINFO(impl_share, smb_fstype)->active && old_shareopts != NULL &&
		strcmp(old_shareopts, shareopts) != 0) {
		needs_reshare = B_TRUE;
		smb_disable_share(impl_share);
	}

	shareopts_dup = strdup(shareopts);

	if (shareopts_dup == NULL)
		return (SA_NO_MEMORY);

	if (old_shareopts != NULL)
		free(old_shareopts);

	FSINFO(impl_share, smb_fstype)->shareopts = shareopts_dup;

	if (needs_reshare)
		smb_enable_share(impl_share);

	return (SA_OK);
}

/*
 * Clears a share's SMB options. Used by libshare to
 * clean up shares that are about to be free()'d.
 */
static void
smb_clear_shareopts(sa_share_impl_t impl_share)
{
	free(FSINFO(impl_share, smb_fstype)->shareopts);
	FSINFO(impl_share, smb_fstype)->shareopts = NULL;
}

static const sa_share_ops_t smb_shareops = {
	.enable_share = smb_enable_share,
	.disable_share = smb_disable_share,

	.validate_shareopts = smb_validate_shareopts,
	.update_shareopts = smb_update_shareopts,
	.clear_shareopts = smb_clear_shareopts,
};

/*
 * Provides a convenient wrapper for determining SMB availability
 */
static boolean_t
smb_available(void)
{

	if (access(SHARING_CMD_PATH, F_OK) != 0)
		return (B_FALSE);

	return (B_TRUE);
}

/*
 * Initializes the SMB functionality of libshare.
 */
void
libshare_smb_init(void)
{
	smb_fstype = register_fstype("smb", &smb_shareops);
}
