#include "git-compat-util.h"
#include "hex.h"
#include "tree.h"
#include "object-name.h"
#include "object-store-ll.h"
#include "commit.h"
#include "alloc.h"
#include "tree-walk.h"
#include "repository.h"
#include "pathspec.h"
#include "environment.h"

const char *tree_type = "tree";

int read_tree_at(struct repository *r,
		 struct tree *tree, struct strbuf *base,
		 int depth,
		 const struct pathspec *pathspec,
		 read_tree_fn_t fn, void *context)
{
	struct tree_desc desc;
	struct name_entry entry;
	struct object_id oid;
	int len, oldlen = base->len;
	enum interesting retval = entry_not_interesting;

	if (depth > max_allowed_tree_depth)
		return error("exceeded maximum allowed tree depth");

	if (repo_parse_tree(r, tree))
		return -1;

	init_tree_desc(&desc, &tree->object.oid, tree->buffer, tree->size);

	while (tree_entry(&desc, &entry)) {
		if (retval != all_entries_interesting) {
			retval = tree_entry_interesting(r->index, &entry,
							base, pathspec);
			if (retval == all_entries_not_interesting)
				break;
			if (retval == entry_not_interesting)
				continue;
		}

		switch (fn(r, &entry.oid, base,
			   entry.path, entry.mode, context)) {
		case 0:
			continue;
		case READ_TREE_RECURSIVE:
			break;
		default:
			return -1;
		}

		if (S_ISDIR(entry.mode)) {
			oidcpy(&oid, &entry.oid);

			len = tree_entry_len(&entry);
			strbuf_add(base, entry.path, len);
			strbuf_addch(base, '/');
			retval = read_tree_at(r, lookup_tree(r, &oid),
						base, 0, pathspec,
						fn, context);
			strbuf_setlen(base, oldlen);
			if (retval)
				return -1;
		} else if (pathspec->recurse_submodules && S_ISGITLINK(entry.mode)) {
			struct commit *commit;
			struct repository subrepo;
			struct repository* subrepo_p = &subrepo;
			struct tree* submodule_tree;
			char *submodule_rel_path;
			int name_base_len = 0;

			len = tree_entry_len(&entry);
			strbuf_add(base, entry.path, len);
			submodule_rel_path = base->buf;
			// repo_submodule_init expects a path relative to submodule_prefix
			if (r->submodule_prefix) {
				name_base_len = strlen(r->submodule_prefix);
				// we should always expect to start with submodule_prefix
				assert(!strncmp(submodule_rel_path, r->submodule_prefix, name_base_len));
				// strip the prefix
				submodule_rel_path += name_base_len;
				// if submodule_prefix doesn't end with a /, we want to get rid of that too
				if (is_dir_sep(submodule_rel_path[0])) {
					submodule_rel_path++;
				}
			}

			if (repo_submodule_init(subrepo_p, r, submodule_rel_path, null_oid()))
				die("couldn't init submodule %s", base->buf);

			if (repo_read_index(subrepo_p) < 0)
				die("index file corrupt");

			commit = lookup_commit(subrepo_p, &entry.oid);
			if (!commit)
				die("Commit %s in submodule path %s not found",
				    oid_to_hex(&entry.oid),
				    base->buf);

			if (repo_parse_commit(subrepo_p, commit))
				die("Invalid commit %s in submodule path %s%s",
				    oid_to_hex(&entry.oid),
				    base->buf);

			submodule_tree = repo_get_commit_tree(subrepo_p, commit);
			oidcpy(&oid, submodule_tree ? &submodule_tree->object.oid : NULL);

			strbuf_addch(base, '/');

			retval = read_tree_at(subrepo_p, lookup_tree(subrepo_p, &oid),
						base, 0, pathspec,
						fn, context);
			if (retval)
			    die("failed to read tree for %s", base->buf);
			strbuf_setlen(base, oldlen);
			repo_clear(subrepo_p);
		}
		// else, this is a file (or a submodule, but no pathspec->recurse_submodules)
	}
	return 0;
}

int read_tree(struct repository *r,
	      struct tree *tree,
	      const struct pathspec *pathspec,
	      read_tree_fn_t fn, void *context)
{
	struct strbuf sb = STRBUF_INIT;
	int ret = read_tree_at(r, tree, &sb, 0, pathspec, fn, context);
	strbuf_release(&sb);
	return ret;
}

int base_name_compare(const char *name1, size_t len1, int mode1,
		      const char *name2, size_t len2, int mode2)
{
	unsigned char c1, c2;
	size_t len = len1 < len2 ? len1 : len2;
	int cmp;

	cmp = memcmp(name1, name2, len);
	if (cmp)
		return cmp;
	c1 = name1[len];
	c2 = name2[len];
	if (!c1 && S_ISDIR(mode1))
		c1 = '/';
	if (!c2 && S_ISDIR(mode2))
		c2 = '/';
	return (c1 < c2) ? -1 : (c1 > c2) ? 1 : 0;
}

/*
 * df_name_compare() is identical to base_name_compare(), except it
 * compares conflicting directory/file entries as equal. Note that
 * while a directory name compares as equal to a regular file, they
 * then individually compare _differently_ to a filename that has
 * a dot after the basename (because '\0' < '.' < '/').
 *
 * This is used by routines that want to traverse the git namespace
 * but then handle conflicting entries together when possible.
 */
int df_name_compare(const char *name1, size_t len1, int mode1,
		    const char *name2, size_t len2, int mode2)
{
	unsigned char c1, c2;
	size_t len = len1 < len2 ? len1 : len2;
	int cmp;

	cmp = memcmp(name1, name2, len);
	if (cmp)
		return cmp;
	/* Directories and files compare equal (same length, same name) */
	if (len1 == len2)
		return 0;
	c1 = name1[len];
	if (!c1 && S_ISDIR(mode1))
		c1 = '/';
	c2 = name2[len];
	if (!c2 && S_ISDIR(mode2))
		c2 = '/';
	if (c1 == '/' && !c2)
		return 0;
	if (c2 == '/' && !c1)
		return 0;
	return c1 - c2;
}

int name_compare(const char *name1, size_t len1, const char *name2, size_t len2)
{
	size_t min_len = (len1 < len2) ? len1 : len2;
	int cmp = memcmp(name1, name2, min_len);
	if (cmp)
		return cmp;
	if (len1 < len2)
		return -1;
	if (len1 > len2)
		return 1;
	return 0;
}

struct tree *lookup_tree(struct repository *r, const struct object_id *oid)
{
	struct object *obj = lookup_object(r, oid);
	if (!obj)
		return create_object(r, oid, alloc_tree_node(r));
	return object_as_type(obj, OBJ_TREE, 0);
}

int parse_tree_buffer(struct tree *item, void *buffer, unsigned long size)
{
	if (item->object.parsed)
		return 0;
	item->object.parsed = 1;
	item->buffer = buffer;
	item->size = size;

	return 0;
}

int repo_parse_tree_gently(struct repository *r, struct tree *item, int quiet_on_missing)
{
	 enum object_type type;
	 void *buffer;
	 unsigned long size;

	if (item->object.parsed)
		return 0;
	buffer = repo_read_object_file(r, &item->object.oid, &type, &size);
	if (!buffer)
		return quiet_on_missing ? -1 :
			error("Could not read %s",
			     oid_to_hex(&item->object.oid));
	if (type != OBJ_TREE) {
		free(buffer);
		return error("Object %s not a tree",
			     oid_to_hex(&item->object.oid));
	}
	return parse_tree_buffer(item, buffer, size);
}

void free_tree_buffer(struct tree *tree)
{
	FREE_AND_NULL(tree->buffer);
	tree->size = 0;
	tree->object.parsed = 0;
}

struct tree *repo_parse_tree_indirect(struct repository *r, const struct object_id *oid)
{
	struct object *obj = parse_object(r, oid);
	return (struct tree *)repo_peel_to_type(r, NULL, 0, obj, OBJ_TREE);
}
