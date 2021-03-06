/* scan-tree.c
 * 
 * Copyright (C) 2008-2009 Lars Hjemli
 * Copyright (C) 2010-2013 Jason A. Donenfeld <Jason@zx2c4.com>
 *
 * Licensed under GNU General Public License v2
 *   (see COPYING for full license text)
 */

#include "cgit.h"
#include "scan-tree.h"
#include "configfile.h"
#include "html.h"

/* return 1 if path contains a objects/ directory and a HEAD file */
static int is_git_dir(const char *path)
{
	struct stat st;
	struct strbuf pathbuf = STRBUF_INIT;
	int result = 0;

	strbuf_addf(&pathbuf, "%s/objects", path);
	if (stat(pathbuf.buf, &st)) {
		if (errno != ENOENT)
			fprintf(stderr, "Error checking path %s: %s (%d)\n",
				path, strerror(errno), errno);
		goto out;
	}
	if (!S_ISDIR(st.st_mode))
		goto out;

	strbuf_reset(&pathbuf);
	strbuf_addf(&pathbuf, "%s/HEAD", path);
	if (stat(pathbuf.buf, &st)) {
		if (errno != ENOENT)
			fprintf(stderr, "Error checking path %s: %s (%d)\n",
				path, strerror(errno), errno);
		goto out;
	}
	if (!S_ISREG(st.st_mode))
		goto out;

	result = 1;
out:
	strbuf_release(&pathbuf);
	return result;
}

struct cgit_repo *repo;
repo_config_fn config_fn;

static void repo_config(const char *name, const char *value)
{
	config_fn(repo, name, value);
}

static int gitconfig_config(const char *key, const char *value, void *cb)
{
	if (!strcmp(key, "gitweb.owner"))
		config_fn(repo, "owner", value);
	else if (!strcmp(key, "gitweb.description"))
		config_fn(repo, "desc", value);
	else if (!strcmp(key, "gitweb.category"))
		config_fn(repo, "section", value);
	else if (!prefixcmp(key, "cgit."))
		config_fn(repo, key + 5, value);

	return 0;
}

static char *xstrrchr(char *s, char *from, int c)
{
	while (from >= s && *from != c)
		from--;
	return from < s ? NULL : from;
}

static void add_repo(const char *base, struct strbuf *path, repo_config_fn fn)
{
	struct stat st;
	struct passwd *pwd;
	size_t pathlen;
	struct strbuf rel = STRBUF_INIT;
	char *p, *slash;
	int n;
	size_t size;

	if (stat(path->buf, &st)) {
		fprintf(stderr, "Error accessing %s: %s (%d)\n",
			path->buf, strerror(errno), errno);
		return;
	}

	strbuf_addch(path, '/');
	pathlen = path->len;

	if (ctx.cfg.strict_export) {
		strbuf_addstr(path, ctx.cfg.strict_export);
		if(stat(path->buf, &st))
			return;
		strbuf_setlen(path, pathlen);
	}

	strbuf_addstr(path, "noweb");
	if (!stat(path->buf, &st))
		return;
	strbuf_setlen(path, pathlen);

	if (strncmp(base, path->buf, strlen(base)))
		strbuf_addbuf(&rel, path);
	else
		strbuf_addstr(&rel, path->buf + strlen(base) + 1);

	if (!strcmp(rel.buf + rel.len - 5, "/.git"))
		strbuf_setlen(&rel, rel.len - 5);
	else if (rel.len && rel.buf[rel.len - 1] == '/')
		strbuf_setlen(&rel, rel.len - 1);

	repo = cgit_add_repo(rel.buf);
	config_fn = fn;
	if (ctx.cfg.enable_git_config) {
		strbuf_addstr(path, "config");
		git_config_from_file(gitconfig_config, path->buf, NULL);
		strbuf_setlen(path, pathlen);
	}

	if (ctx.cfg.remove_suffix)
		if ((p = strrchr(repo->url, '.')) && !strcmp(p, ".git"))
			*p = '\0';
	repo->path = xstrdup(path->buf);
	while (!repo->owner) {
		if ((pwd = getpwuid(st.st_uid)) == NULL) {
			fprintf(stderr, "Error reading owner-info for %s: %s (%d)\n",
				path->buf, strerror(errno), errno);
			break;
		}
		if (pwd->pw_gecos)
			if ((p = strchr(pwd->pw_gecos, ',')))
				*p = '\0';
		repo->owner = xstrdup(pwd->pw_gecos ? pwd->pw_gecos : pwd->pw_name);
	}

	if (repo->desc == cgit_default_repo_desc || !repo->desc) {
		strbuf_addstr(path, "description");
		if (!stat(path->buf, &st))
			readfile(path->buf, &repo->desc, &size);
		strbuf_setlen(path, pathlen);
	}

	if (ctx.cfg.section_from_path) {
		n  = ctx.cfg.section_from_path;
		if (n > 0) {
			slash = rel.buf;
			while (slash && n && (slash = strchr(slash, '/')))
				n--;
		} else {
			slash = rel.buf + rel.len;
			while (slash && n && (slash = xstrrchr(rel.buf, slash, '/')))
				n++;
		}
		if (slash && !n) {
			*slash = '\0';
			repo->section = xstrdup(rel.buf);
			*slash = '/';
			if (!prefixcmp(repo->name, repo->section)) {
				repo->name += strlen(repo->section);
				if (*repo->name == '/')
					repo->name++;
			}
		}
	}

	strbuf_addstr(path, "cgitrc");
	if (!stat(path->buf, &st))
		parse_configfile(xstrdup(path->buf), &repo_config);

	strbuf_release(&rel);
}

static void scan_path(const char *base, const char *path, repo_config_fn fn)
{
	DIR *dir = opendir(path);
	struct dirent *ent;
	struct strbuf pathbuf = STRBUF_INIT;
	size_t pathlen = strlen(path);
	struct stat st;

	if (!dir) {
		fprintf(stderr, "Error opening directory %s: %s (%d)\n",
			path, strerror(errno), errno);
		return;
	}

	strbuf_add(&pathbuf, path, strlen(path));
	if (is_git_dir(pathbuf.buf)) {
		add_repo(base, &pathbuf, fn);
		goto end;
	}
	strbuf_addstr(&pathbuf, "/.git");
	if (is_git_dir(pathbuf.buf)) {
		add_repo(base, &pathbuf, fn);
		goto end;
	}
	/*
	 * Add one because we don't want to lose the trailing '/' when we
	 * reset the length of pathbuf in the loop below.
	 */
	pathlen++;
	while ((ent = readdir(dir)) != NULL) {
		if (ent->d_name[0] == '.') {
			if (ent->d_name[1] == '\0')
				continue;
			if (ent->d_name[1] == '.' && ent->d_name[2] == '\0')
				continue;
			if (!ctx.cfg.scan_hidden_path)
				continue;
		}
		strbuf_setlen(&pathbuf, pathlen);
		strbuf_addstr(&pathbuf, ent->d_name);
		if (stat(pathbuf.buf, &st)) {
			fprintf(stderr, "Error checking path %s: %s (%d)\n",
				pathbuf.buf, strerror(errno), errno);
			continue;
		}
		if (S_ISDIR(st.st_mode))
			scan_path(base, pathbuf.buf, fn);
	}
end:
	strbuf_release(&pathbuf);
	closedir(dir);
}

#define lastc(s) s[strlen(s) - 1]

void scan_projects(const char *path, const char *projectsfile, repo_config_fn fn)
{
	struct strbuf line = STRBUF_INIT;
	FILE *projects;
	int err;

	projects = fopen(projectsfile, "r");
	if (!projects) {
		fprintf(stderr, "Error opening projectsfile %s: %s (%d)\n",
			projectsfile, strerror(errno), errno);
		return;
	}
	while (strbuf_getline(&line, projects, '\n') != EOF) {
		if (!line.len)
			continue;
		strbuf_insert(&line, 0, "/", 1);
		strbuf_insert(&line, 0, path, strlen(path));
		scan_path(path, line.buf, fn);
	}
	if ((err = ferror(projects))) {
		fprintf(stderr, "Error reading from projectsfile %s: %s (%d)\n",
			projectsfile, strerror(err), err);
	}
	fclose(projects);
	strbuf_release(&line);
}

void scan_tree(const char *path, repo_config_fn fn)
{
	scan_path(path, path, fn);
}
