/*
 * Extended attribute operations.
 *
 * Copyright (c) 2008-2014 Daniel Phillips
 * Copyright (c) 2008-2014 OGAWA Hirofumi
 */

#include "tux3.h"
#include "iattr.h"

#ifndef trace
#define trace trace_on
#endif

/* Xattr Atoms */

/*
 * Atom count table:
 *
 * * Both tables are mapped into the atom table at a high logical offset.
 *   Allowing 32 bits worth of atom numbers, and with at lest 256 bytes
 *   per atom entry, we need about (1 << 32 + 8) = 1 TB dirent bytes
 *   for the atom dictionary, so the refcount tables start at block
 *   2^40 >> 12 = 2^28.
 *
 * * The refcount table consists of pairs of blocks: even blocks with the low
 *   16 bits of refcount and odd blocks with the high 16 bits.  For 2^32 atoms
 *   that is 2^34 bytes at most, or 2^22 4K blocks.
 *
 * Atom reverse map:
 *
 * * When a new atom dirent is created we also set the reverse map for the
 *   dirent's atom number to the file offset at which the dirent was created.
 *   This will be 64 bits just to be lazy so that is 2^32 atoms * 8 bytes
 *   = 2^35 revmap bytes = 2^23 4K blocks. This starts just above the count
 *   table, which puts it at logical offset 2^28 + 2^23, leaving a gap after
 *   the count table in case we decide 32 bits of ref count is not enough.
 */

typedef u32 atom_t;

/* see dir.c */
#define HEAD_ALIGN		sizeof(inum_t)
#define HEAD_SIZE		offsetof(struct tux3_dirent, name)
/* FIXME: probably, we should limit maximum name length */
#define MAX_ATOM_NAME_LEN	(256 - HEAD_SIZE)

#define MAX_ATABLE_SIZE_BITS	48
#define ATOM_DICT_BITS		40
#define ATOMREF_TABLE_BITS	34

/*
 * FIXME: refcount bits is too small in theory. Because maximum
 * refcount is maximum inodes.
 */
#define ATOMREF_SIZE		2
#define ATOMREF_BLKBITS		1

#define UNATOM_SIZE		8
#define UNATOM_BLKBITS		3
/* Sign bit is used for error */
#define UNATOM_FREE_MAGIC	(0x6eadfceeULL << (sizeof(atom_t) * 8))
#define UNATOM_FREE_MASK	(0xffffffffULL << (sizeof(atom_t) * 8))

/* Initialize base address for dictionaries on atable */
void atable_init_base(struct sb *sb)
{
	sb->atomref_base = (block_t)1 << (ATOM_DICT_BITS - sb->blockbits);
	sb->unatom_base = sb->atomref_base
		+ ((block_t)1 << (ATOMREF_TABLE_BITS - sb->blockbits));
}

static inline atom_t entry_atom(struct tux3_dirent *entry)
{
	return be64_to_cpu(entry->inum);
}

static struct buffer_head *blockread_unatom(struct inode *atable, atom_t atom,
					    unsigned *offset)
{
	struct sb *sb = tux_sb(atable->i_sb);
	unsigned shift = sb->blockbits - UNATOM_BLKBITS;

	*offset = atom & ((1U << shift) - 1);
	return blockread(mapping(atable), sb->unatom_base + (atom >> shift));
}

static loff_t unatom_dict_read(struct inode *atable, atom_t atom)
{
	struct buffer_head *buffer;
	unsigned offset;

	buffer = blockread_unatom(atable, atom, &offset);
	if (!buffer)
		return -EIO;

	__be64 *unatom_dict = bufdata(buffer);
	loff_t where = be64_to_cpu(unatom_dict[offset]);
	blockput(buffer);

	return where;
}

static loff_t unatom_dict_write(struct inode *atable, atom_t atom, loff_t where)
{
	unsigned delta = tux3_get_current_delta();
	struct buffer_head *buffer, *clone;
	loff_t old;
	unsigned offset;

	buffer = blockread_unatom(atable, atom, &offset);
	if (!buffer)
		return -EIO;

	/*
	 * The atable is protected by inode_lock for now.
	 * blockdirty() should never return -EAGAIN.
	 * FIXME: need finer granularity locking
	 */
	clone = blockdirty(buffer, delta);
	if (IS_ERR(clone)) {
		assert(PTR_ERR(clone) != -EAGAIN);
		blockput(buffer);
		return PTR_ERR(clone);
	}

	__be64 *unatom_dict = bufdata(clone);
	old = be64_to_cpu(unatom_dict[offset]);
	unatom_dict[offset] = cpu_to_be64(where);
	mark_buffer_dirty_non(clone);
	blockput(clone);

	return old;
}

static int is_free_unatom(loff_t where)
{
	return (where & UNATOM_FREE_MASK) == UNATOM_FREE_MAGIC;
}

/* Convert atom to name */
static int unatom(struct inode *atable, atom_t atom, char *name, unsigned size)
{
	struct sb *sb = tux_sb(atable->i_sb);
	struct buffer_head *buffer;
	int err;

	loff_t where = unatom_dict_read(atable, atom);
	if (where < 0) {
		err = where;
		goto error;
	}

	buffer = blockread(mapping(atable), where >> sb->blockbits);
	if (!buffer) {
		err = -EIO;
		goto error;
	}
	struct tux3_dirent *entry = bufdata(buffer) + (where & sb->blockmask);
	if (entry_atom(entry) != atom) {
		tux3_fs_error(sb, "atom %x reverse entry broken", atom);
		err = -EIO;
		goto error_blockput;
	}
	unsigned len = entry->name_len;
	if (size) {
		if (len > size) {
			err = -ERANGE;
			goto error_blockput;
		}
		memcpy(name, entry->name, len);
	}
	blockput(buffer);

	return len;

error_blockput:
	blockput(buffer);
error:
	return err;
}

/* Find free atom */
static int get_freeatom(struct inode *atable, atom_t *atom)
{
	struct sb *sb = tux_sb(atable->i_sb);
	atom_t freeatom = sb->freeatom;

	if (!freeatom) {
		*atom = sb->atomgen++;
		return 0;
	}

	loff_t next = unatom_dict_read(atable, freeatom);
	if (next < 0)
		return next;
	if (!is_free_unatom(next)) {
		tux3_fs_error(sb, "something horrible happened");
		return -EIO;
	}

	*atom = freeatom;
	sb->freeatom = next & ~UNATOM_FREE_MASK;

	return 0;
}

/* Find atom of name */
static int find_atom(struct inode *atable, const char *name, unsigned len,
		     atom_t *atom)
{
	struct sb *sb = tux_sb(atable->i_sb);
	struct buffer_head *buffer;
	struct tux3_dirent *entry;

	entry = tux_find_entry(atable, name, len, &buffer, sb->atomdictsize);
	if (IS_ERR(entry)) {
		int err = PTR_ERR(entry);
		if (err == -ENOENT)
			return -ENODATA;
		return err;
	}

	*atom = entry_atom(entry);
	blockput(buffer);
	return 0;
}

/* Make atom for name */
static int make_atom(struct inode *atable, const char *name, unsigned len,
		     atom_t *atom)
{
	struct sb *sb = tux_sb(atable->i_sb);
	struct buffer_head *buffer;
	loff_t where;
	int err;

	err = find_atom(atable, name, len, atom);
	if (!err)
		return 0;
	if (err != -ENODATA)
		return err;

	err = get_freeatom(atable, atom);
	if (err)
		return err;

	where = tux_alloc_entry(atable, name, len, &sb->atomdictsize, &buffer);
	if (where < 0) {
		/* FIXME: better set a flag that unatom broke or something!!! */
		return where;
	}
	/* This releases buffer */
	tux_set_entry(buffer, bufdata(buffer) + (where & sb->blockmask),
		      *atom, 0);

	/* Enter into reverse map - maybe verify zero refs? */
	where = unatom_dict_write(atable, *atom, where);
	if (where < 0) {
		/* FIXME: better set a flag that unatom broke or something!!! */
		return where;
	}

	return 0;
}

/* Modify buffer of refcount, then release buffer */
static int update_refcount(struct sb *sb, struct buffer_head *buffer,
			   unsigned offset, u16 val)
{
	unsigned delta = tux3_get_current_delta();
	struct buffer_head *clone;
	__be16 *refcount;

	/*
	 * The atable is protected by inode_lock for now.
	 * blockdirty() should never return -EAGAIN.
	 * FIXME: need finer granularity locking
	 */
	clone = blockdirty(buffer, delta);
	if (IS_ERR(clone)) {
		assert(PTR_ERR(clone) != -EAGAIN);
		blockput(buffer);
		return PTR_ERR(clone);
	}

	refcount = bufdata(clone);
	refcount[offset] = cpu_to_be16(val);
	mark_buffer_dirty_non(clone);
	blockput(clone);

	return 0;
}

/* Modify atom refcount */
static int atomref(struct inode *atable, atom_t atom, int use)
{
	struct sb *sb = tux_sb(atable->i_sb);
	unsigned shift = sb->blockbits - ATOMREF_BLKBITS;
	block_t block = sb->atomref_base + ATOMREF_SIZE * (atom >> shift);
	unsigned offset = atom & ((1U << shift) - 1), kill = 0;
	struct buffer_head *buffer;
	__be16 *refcount;
	int err;

	buffer = blockread(mapping(atable), block);
	if (!buffer)
		return -EIO;

	refcount = bufdata(buffer);
	int low = be16_to_cpu(refcount[offset]) + use;
	trace("inc atom %x by %d, offset %llx[%x], low = %d",
	      atom, use, block, offset, low);

	/* This releases buffer */
	err = update_refcount(sb, buffer, offset, low);
	if (err)
		return err;

	if (!low || (low & (~0U << 16))) {
		buffer = blockread(mapping(atable), block + 1);
		if (!buffer)
			return -EIO;

		refcount = bufdata(buffer);
		int high = be16_to_cpu(refcount[offset]);
		if (!low)
			blockput(buffer);
		else {
			trace("carry %d, offset %llx[%x], high = %d",
			      (low >> 16), block, offset, high);
			high += (low >> 16);
			assert(high >= 0); /* paranoia check */

			/* This releases buffer */
			err = update_refcount(sb, buffer, offset, high);
			if (err) {
				/* FIXME: better set a flag that atomref broke
				 * or something! */
				return err;
			}
		}

		kill = !(low | high);
	}

	if (kill) {
		trace("delete atom %x", atom);
		loff_t next = UNATOM_FREE_MAGIC | sb->freeatom;
		loff_t where = unatom_dict_write(atable, atom, next);
		if (where < 0) {
			/* FIXME: better set a flag that unatom broke
			 * or something! */
			return -EIO;
		}
		sb->freeatom = atom;

		buffer = blockread(mapping(atable), where >> sb->blockbits);
		if (!buffer) {
			/* FIXME: better set a flag that unatom broke
			 * or something! */
			return -EIO;
		}

		struct tux3_dirent *entry = bufdata(buffer) + (where & sb->blockmask);
		if (entry_atom(entry) == atom) {
			/* FIXME: better set a flag that unatom broke
			 * or something! */
			err = tux_delete_entry(atable, buffer, entry);
			if (err)
				return err;
		} else {
			/* FIXME: better set a flag that unatom broke
			 * or something! */
			/* Corruption of refcount or something */
			tux3_fs_error(sb, "atom entry not found");
			blockput(buffer);
			return -EIO;
		}
	}

	return 0;
}

/* userland only */
void dump_atoms(struct inode *atable)
{
	struct sb *sb = tux_sb(atable->i_sb);
	unsigned blocks = (sb->atomgen + (sb->blockmask >> ATOMREF_BLKBITS))
		>> (sb->blockbits - ATOMREF_BLKBITS);

	unsigned j;
	for (j = 0; j < blocks; j++) {
		block_t block = sb->atomref_base + ATOMREF_SIZE * j;
		struct buffer_head *lobuf, *hibuf;
		lobuf = blockread(mapping(atable), block);
		if (!lobuf)
			goto eek;
		hibuf = blockread(mapping(atable), block + 1);
		if (!hibuf) {
			blockput(lobuf);
			goto eek;
		}
		__be16 *lorefs = bufdata(lobuf), *hirefs = bufdata(hibuf);
		unsigned i;
		for (i = 0; i < (sb->blocksize >> ATOMREF_BLKBITS); i++) {
			unsigned refs = (be16_to_cpu(hirefs[i]) << 16) | be16_to_cpu(lorefs[i]);
			if (!refs)
				continue;
			atom_t atom = i;
			char name[100];
			int len = unatom(atable, atom, name, sizeof(name));
			if (len < 0)
				goto eek;
			__tux3_dbg("%.*s: atom 0x%08x, ref %u\n",
				   len, name, atom, refs);
		}
		blockput(lobuf);
		blockput(hibuf);
	}
	return;

eek:
	tux3_err(sb, "atom name lookup failed");
}

/* userland only */
void show_freeatoms(struct sb *sb)
{
	struct inode *atable = sb->atable;
	atom_t atom = sb->freeatom;

	while (atom) {
		tux3_dbg("free atom: %x", atom);
		loff_t next = unatom_dict_read(atable, atom);
		if (next < 0)
			goto eek;
		if (!is_free_unatom(next))
			goto eek;
		atom = next & ~UNATOM_FREE_MASK;
	}
	return;

eek:
	tux3_err(sb, "eek");
}

/* Xattr cache */

/* Note, this structure may not be aligned, so use {get,put}_unaligned(). */
struct xcache_entry {
	/* FIXME: 16bits? */
	u16 atom;		/* atom of xattr data */
	u16 size;		/* size of body[] */
	char body[];
};

struct xcache {
	u16 size;		/* size of xattrs[] */
	u16 maxsize;		/* allocated memory size */
	struct xcache_entry xattrs[];
};

/* Free xcache memory */
void free_xcache(struct inode *inode)
{
	if (tux_inode(inode)->xcache) {
		kfree(tux_inode(inode)->xcache);
		tux_inode(inode)->xcache = NULL;
	}
}

/* Allocate new xcache memory */
int new_xcache(struct inode *inode, unsigned size)
{
	struct xcache *xcache;

	xcache = kmalloc(sizeof(*xcache) + size, GFP_NOFS);
	if (!xcache)
		return -ENOMEM;

	xcache->size = 0;
	xcache->maxsize = size;
	tux_inode(inode)->xcache = xcache;

	return 0;
}

/* Expand xcache memory */
static int expand_xcache(struct inode *inode, unsigned size)
{
#define MIN_ALLOC_SIZE	(1 << 7)
	struct xcache *xcache, *old = tux_inode(inode)->xcache;

	assert(!old || size > old->maxsize);

	/* FIXME: better allocation strategy? See xcache_update() comment */
	if (old)
		size = max_t(unsigned, old->maxsize * 2, size);
	else
		size = ALIGN(size, MIN_ALLOC_SIZE);
	trace("realloc xcache to %i", size);

	assert(size);
	assert(size <= USHRT_MAX);

	xcache = kmalloc(sizeof(*xcache) + size, GFP_NOFS);
	if (!xcache)
		return -ENOMEM;

	if (!old)
		xcache->size = 0;
	else {
		xcache->size = old->size;
		memcpy(xcache->xattrs, old->xattrs, old->size);
		kfree(old);
	}
	xcache->maxsize = size;

	tux_inode(inode)->xcache = xcache;

	return 0;
}

static inline void xcache_fill_entry(struct xcache_entry *xattr, u16 atom,
				     const void *data, u16 size)
{
	put_unaligned(atom, &xattr->atom);
	put_unaligned(size, &xattr->size);
	memcpy(xattr->body, data, size);
}

static inline struct xcache_entry *xcache_next(struct xcache_entry *xattr)
{
	return (void *)xattr->body + get_unaligned(&xattr->size);
}

static inline struct xcache_entry *xcache_limit(struct xcache *xcache)
{
	return (void *)xcache->xattrs + xcache->size;
}

int xcache_dump(struct inode *inode)
{
	struct xcache *xcache = tux_inode(inode)->xcache;

	if (!xcache)
		return 0;

	struct xcache_entry *xattr = xcache->xattrs;
	struct xcache_entry *xlimit = xcache_limit(xcache);

	//__tux3_dbg("xattrs %p/%i", inode->xcache, inode->xcache->size);
	while (xattr < xlimit) {
		u16 size = get_unaligned(&xattr->size);
		if (size > tux_sb(inode->i_sb)->blocksize)
			goto bail;
		__tux3_dbg("atom %.3x => ", get_unaligned(&xattr->atom));
		if (size)
			hexdump(xattr->body, size);
		else
			__tux3_dbg("<empty>\n");
		xattr = xcache_next(xattr);
		if (xattr > xlimit)
			goto fail;
	}
	assert(xattr == xlimit);
	return 0;

fail:
	tux3_err(tux_sb(inode->i_sb), "corrupt xattrs");
	return -1;
bail:
	tux3_err(tux_sb(inode->i_sb), "xattr too big");
	return -1;
}

static struct xcache_entry *xcache_lookup(struct xcache *xcache, unsigned atom)
{
	if (xcache) {
		struct xcache_entry *xattr = xcache->xattrs;
		struct xcache_entry *xlimit = xcache_limit(xcache);
		while (xattr < xlimit) {
			if (get_unaligned(&xattr->atom) == atom)
				return xattr;
			xattr = xcache_next(xattr);
			if (xattr > xlimit)
				return ERR_PTR(-EINVAL);
		}
		assert(xattr == xlimit);
	}
	return ERR_PTR(-ENOATTR);
}

static inline int remove_old(struct xcache *xcache, struct xcache_entry *xattr)
{
	if (xattr) {
		void *xlimit = xcache_limit(xcache);
		void *next = xcache_next(xattr);
		memmove(xattr, next, xlimit - next);
		xcache->size -= next - (void *)xattr;
		return 1;
	}
	return 0;
}

/*
 * Things to improve about xcache_update:
 *
 *  * It always allocates the new attribute at the end of the list because it
 *    is lazy and works by always deleting the attribute first then putting
 *    the new one at the end
 *
 *  * If the size of the attribute did not change, does unecessary work
 *
 *  * Should expand by binary factor
 */
static int xcache_update(struct inode *inode, unsigned atom, const void *data,
			 unsigned len, unsigned flags)
{
	struct xcache *xcache = tux_inode(inode)->xcache;
	struct xcache_entry *xattr = xcache_lookup(xcache, atom);
	int use = 0;

	if (IS_ERR(xattr)) {
		if (PTR_ERR(xattr) != -ENOATTR || (flags & XATTR_REPLACE))
			return PTR_ERR(xattr);

		tux3_xattrdirty(inode);
	} else {
		if (flags & XATTR_CREATE)
			return -EEXIST;

		tux3_xattrdirty(inode);
		/* FIXME: if we can't insert new one, the xattr will lose */
		use -= remove_old(xcache, xattr);
	}

	/* Insert new */
	unsigned more = sizeof(*xattr) + len;
	if (!xcache || xcache->size + more > xcache->maxsize) {
		unsigned oldsize = xcache ? xcache->size : 0;
		int err = expand_xcache(inode, oldsize + more);
		if (err)
			return err;
	}
	xattr = xcache_limit(tux_inode(inode)->xcache);
	//trace("expand by %i\n", more);
	tux_inode(inode)->xcache->size += more;
	xcache_fill_entry(xattr, atom, data, len);

	tux3_iattrdirty(inode);
	inode->i_ctime = current_time(inode);
	tux3_mark_inode_dirty(inode);

	use++;
	if (use) {
		/* FIXME: error check */
		atomref(tux_sb(inode->i_sb)->atable, atom, use);
	}

	return 0;
}

/* Inode is going to purge, remove xattrs */
int xcache_remove_all(struct inode *inode)
{
	struct sb *sb = tux_sb(inode->i_sb);
	struct xcache *xcache = tux_inode(inode)->xcache;

	if (xcache) {
		struct xcache_entry *xattr = xcache->xattrs;
		struct xcache_entry *xlimit = xcache_limit(xcache);
		int err = 0;

		inode_lock(sb->atable);
		while (xattr < xlimit) {
			/*
			 * FIXME: Inode is going to purse, what to do
			 * if error ?
			 */
			atom_t atom = get_unaligned(&xattr->atom);
			err = atomref(sb->atable, atom, -1);
			if (err)
				break;

			xattr = xcache_next(xattr);
		}
		inode_unlock(sb->atable);
		if (err)
			return err;
		assert(xattr == xlimit);
	}

	free_xcache(inode);

	return 0;
}

int get_xattr(struct inode *inode, const char *name, unsigned len, void *data,
	      unsigned size)
{
	struct inode *atable = tux_sb(inode->i_sb)->atable;
	atom_t atom;
	int ret;

	inode_lock_shared(atable);
	ret = find_atom(atable, name, len, &atom);
	if (ret)
		goto out;

	struct xcache *xcache = tux_inode(inode)->xcache;
	struct xcache_entry *xattr = xcache_lookup(xcache, atom);
	if (IS_ERR(xattr)) {
		ret = PTR_ERR(xattr);
		goto out;
	}
	ret = get_unaligned(&xattr->size);
	if (ret <= size)
		memcpy(data, xattr->body, ret);
	else if (size)
		ret = -ERANGE;
out:
	inode_unlock_shared(atable);
	return ret;
}

int set_xattr(struct inode *inode, const char *name, unsigned len,
	      const void *data, unsigned size, unsigned flags)
{
	struct sb *sb = tux_sb(inode->i_sb);
	struct inode *atable = sb->atable;

	inode_lock(atable);
	/* FIXME: cost */
	change_begin(sb, 0);

	atom_t atom;
	int err = make_atom(atable, name, len, &atom);
	if (!err) {
		err = xcache_update(inode, atom, data, size, flags);
		if (err) {
			/* FIXME: maybe, recovery for make_atom */
		}
	}

	change_end(sb);
	inode_unlock(atable);

	return err;
}

int del_xattr(struct inode *inode, const char *name, unsigned len)
{
	struct sb *sb = tux_sb(inode->i_sb);
	struct inode *atable = sb->atable;
	int err;

	inode_lock(atable);
	/* FIXME: cost */
	change_begin(sb, 0);

	atom_t atom;
	err = find_atom(atable, name, len, &atom);
	if (!err) {
		struct xcache *xcache = tux_inode(inode)->xcache;
		struct xcache_entry *xattr = xcache_lookup(xcache, atom);
		if (IS_ERR(xattr)) {
			err = PTR_ERR(xattr);
			goto out;
		}

		tux3_xattrdirty(inode);
		int used = remove_old(xcache, xattr);
		if (used) {
			tux3_iattrdirty(inode);
			inode->i_ctime = current_time(inode);
			tux3_mark_inode_dirty(inode);
			/* FIXME: error check */
			atomref(atable, atom, -used);
		}
	}
out:
	change_end(sb);
	inode_unlock(atable);

	return err;
}

int list_xattr(struct inode *inode, char *text, size_t size)
{
	struct sb *sb = tux_sb(inode->i_sb);
	struct inode *atable = sb->atable;

	inode_lock_shared(atable);

	struct xcache *xcache = tux_inode(inode)->xcache;
	if (!xcache)
		return 0;

	struct xcache_entry *xattr = xcache->xattrs;
	struct xcache_entry *xlimit = xcache_limit(xcache);
	char *base = text, *top = text + size;
	int err;

	while (xattr < xlimit) {
		atom_t atom = get_unaligned(&xattr->atom);
		if (size) {
			/* FIXME: check error code for POSIX */
			int tail = top - text;
			int len = unatom(atable, atom, text, tail);
			if (len < 0) {
				err = len;
				goto error;
			}
			if (len == tail) {
				err = -ERANGE;
				goto error;
			}

			*(text += len) = 0;
			text++;
		} else {
			int len = unatom(atable, atom, NULL, 0);
			if (len < 0) {
				err = len;
				goto error;
			}
			text += len + 1;
		}

		xattr = xcache_next(xattr);
		if (xattr > xlimit) {
			tux3_fs_error(sb, "xcache bug");
			err = -EIO;
			goto error;
		}
	}
	assert(xattr == xlimit);
	inode_unlock_shared(atable);

	return text - base;

error:
	inode_unlock_shared(atable);
	return err;
}

/* Xattr encode/decode */

unsigned encode_xsize(struct inode *inode)
{
	struct xcache *xcache = tux_inode(inode)->xcache;

	if (!xcache)
		return 0;

	unsigned xsize = 0, xatsize = atsize[XATTR_ATTR];
	struct xcache_entry *xattr = xcache->xattrs;
	struct xcache_entry *xlimit = xcache_limit(xcache);

	while (xattr < xlimit) {
		xsize += KIND_SIZE + xatsize + get_unaligned(&xattr->size);
		xattr = xcache_next(xattr);
	}
	assert(xattr == xlimit);
	return xsize;
}

void *encode_xattrs(struct inode *inode, void *attrs, unsigned xsize)
{
	struct xcache *xcache = tux_inode(inode)->xcache;

	if (!xcache)
		return attrs;

	struct xcache_entry *xattr = xcache->xattrs;
	struct xcache_entry *xlimit = xcache_limit(xcache);
	void *limit = attrs + xsize - 3;

	while (xattr < xlimit) {
		u16 size, atom;

		if (attrs >= limit)
			break;

		atom = get_unaligned(&xattr->atom);
		size = get_unaligned(&xattr->size);
		//immediate xattr: kind+version:16, bytes:16, atom:16, data[bytes - 2]
		//printf("xattr %x/%x ", atom, size);
		attrs = encode_kind(attrs, XATTR_ATTR, tux_sb(inode->i_sb)->version);
		attrs = encode16(attrs, size + 2);
		attrs = encode16(attrs, atom);
		memcpy(attrs, xattr->body, size);
		attrs += size;
		xattr = xcache_next(xattr);
	}
	return attrs;
}

unsigned decode_xsize(struct inode *inode, void *attrs, unsigned xsize)
{
	struct sb *sb = tux_sb(inode->i_sb);
	unsigned total = 0, bytes;
	void *limit = attrs + xsize;

	while (attrs < limit - 1) {
		unsigned kind, version;
		attrs = decode_kind(attrs, &kind, &version);
		switch (kind) {
		case XATTR_ATTR:
		case IDATA_ATTR:
			// immediate data: kind+version:16, bytes:16, data[bytes]
			// immediate xattr: kind+version:16, bytes:16, atom:16, data[bytes - 2]
			attrs = decode16(attrs, &bytes);
			attrs += bytes;
			if (version == sb->version)
				total += sizeof(struct xcache_entry) + bytes - 2;
			continue;
		}
		attrs += atsize[kind];
	}
	return total;
}

void *decode_xattr(struct inode *inode, void *attrs)
{
	// immediate xattr: kind+version:16, bytes:16, atom:16, data[bytes - 2]
	struct xcache *xcache = tux_inode(inode)->xcache;
	struct xcache_entry *xattr = xcache_limit(xcache);
	void *limit = xcache->xattrs + xcache->maxsize;
	unsigned xsize, bytes, atom;
	u16 size;

	attrs = decode16(attrs, &bytes);
	attrs = decode16(attrs, &atom);

	/* FIXME: check limit!!! */
	assert((void *)xattr + sizeof(*xattr) <= limit);
	size = bytes - 2;
	xcache_fill_entry(xattr, atom, attrs, size);
	xsize = sizeof(*xattr) + size;
	assert((void *)xattr + xsize <= limit);

	attrs += size;
	xcache->size += xsize;

	return attrs;
}
