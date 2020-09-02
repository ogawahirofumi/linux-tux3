/*
 * Tux3 versioning filesystem in user space
 *
 * Original copyright (c) 2008 Daniel Phillips <phillips@phunq.net>
 * Licensed under the GPL version 3
 *
 * By contributing changes to this file you grant the original copyright holder
 * the right to distribute those changes under any license.
 */

#include "tux3user.h"
#include "diskio.h"

#include "tux3_fsck.c"
#include "tux3_image.c"
#include "tux3_dump.c"
#include "tux3_graph.c"

#define VERSION 0.0

static int open_volume(const char *volname)
{
	int fd = open(volname, O_RDWR);
	if (fd < 0)
		strerror_exit(1, errno, "could not open '%s'", volname);
	return fd;
}

static int open_fs(const char *volname, struct sb *sb)
{
	sb_dev(sb)->fd = open_volume(volname);
	return load_fs(sb, 1);
}

static void usage(struct options *options, const char *progname,
		  const char *cmdname, const char *name, const char *blurb)
{
	int cols = 80, tabs[] = { 3, 40, cols < 60 ? 60 : cols };
	char lead[300], help[3000] = {};

	if (cmdname)
		snprintf(lead, sizeof(lead), "Usage: %s %s %s%s",
			 progname, cmdname, name, blurb ? : "");
	else
		snprintf(lead, sizeof(lead), "Usage: %s %s%s",
			 progname, name, blurb ? : "");

	opthelp(help, sizeof(help), options, tabs, lead, !blurb);
	printf("%s\n", help);
}

static struct options onlyhelp[] = {
	{ "verbose", "v", OPT_MANY, "Verbose output", },
	{ "usage", "", 0, "Show usage", },
	{ "help", "?", 0, "Show help", },
	{},
};

struct vars {
	const char *volname;
	unsigned blocksize;
	loff_t seek;
	int verbose;
};

static int common_parse(int *argc, const char ***args, struct options *options,
		int need, const char *progname, const char *cmdname,
		const char *blurb, const char **volname)
{
	unsigned space = optspace(options, *argc, *args);
	void *optv = malloc(space);
	if (!optv)
		strerror_exit(1, errno, "malloc");

	int optc = optscan(options, argc, args, optv, space);
	if (optc < 0)
		error_exit("%s!", opterror(optv));

	if (*argc != need) {
		usage(options, progname, cmdname, blurb, NULL);
		exit(1);
	}

	assert(need > 2);
	*volname = (*args)[2];
	return optc;
}

static void common_options(int *argc, const char ***args,
		struct options *options, int need, const char *progname,
		const char *cmdname, const char *blurb, struct vars *vars)
{
	int optc = common_parse(argc, args, options, need, progname,
				cmdname, blurb, &vars->volname);

	void *optv = argv2optv(*args);

	for (int i = 0; i < optc; i++) {
		const char *value = optvalue(optv, i);
		switch (options[optindex(optv, i)].terse[0]) {
		case 'b':
			vars->blocksize = strtoul(value, NULL, 0);
			break;
		case 's':
			vars->seek = strtoull(value, NULL, 0);
			break;
		case 'v':
			vars->verbose++;
			break;
		case '?':
			usage(options, progname, cmdname, blurb, " [OPTIONS]");
			exit(0);
		case 0:
			usage(options, progname, cmdname, blurb, NULL);
			exit(0);
		}
	}
}

static int cmd_mkfs(struct sb *sb, const char *progname, const char *command,
		    int argc, const char **args)
{
	struct options options[] = {
		{ "blocksize", "b", OPT_NUMBER, "Set block size", },
		{ "verbose", "v", OPT_MANY, "Verbose output", },
		{ "usage", "", 0, "Show usage", },
		{ "help", "?", 0, "Show help", },
		{},
	};
	struct vars vars = { .blocksize = 1 << 12, .verbose = 0 };

	common_options(&argc, &args, options, 3, progname, command,
		       "<volume>", &vars);

	printf("Make tux3 filesystem on %s (blocksize %u)\n",
	       vars.volname, vars.blocksize);

	const char *volname = vars.volname;
	int blocksize = vars.blocksize;
	int fd = open_volume(volname);

	loff_t volsize = 0;
	if (fdsize64(fd, &volsize))
		strerror_exit(1, errno, "fdsize64 failed for '%s'", volname);

	printf("Volume size = %Lu bytes\n", (s64)volsize);

	int blockbits = ffs(blocksize) - 1;
	if (1 << blockbits != blocksize)
		error_exit("blocksize must be a power of two");

	sb_dev(sb)->fd = fd;
	sb_dev(sb)->bits = blockbits;
	sb->super = INIT_DISKSB(blockbits, volsize >> blockbits);

	int err = mkfs_tux3(sb);

	free(argv2optv(args));

	return err;
}

static int cmd_fsck(struct sb *sb, const char *progname, const char *command,
		    int argc, const char **args)
{
	struct vars vars = {};
	int err;

	common_options(&argc, &args, onlyhelp, 3, progname, "fsck",
		       "<volume>", &vars);

	sb_dev(sb)->fd = open_volume(vars.volname);

	err = fsck_main(sb);

	free(argv2optv(args));
	return err;
}

static int cmd_dump(struct sb *sb, const char *progname, const char *command,
		    int argc, const char **args)
{
	struct options options[] = {
		{ "dump_block", "b", OPT_HASARG,
		  "Set filename for block number dump", },
		{ "stats", "s", OPT_MANY, "Statistics", },
		{ "usage", "", 0, "Show usage", },
		{ "help", "?", 0, "Show help", },
		{},
	};
	struct dump_opts opts = {};
	const char *volname;
	int err;

	const char *blurb = "<volname>";
	int optc = common_parse(&argc, &args, options, 3, progname,
				command, blurb, &volname);

	void *optv = argv2optv(args);

	for (int i = 0; i < optc; i++) {
		const char *value = optvalue(optv, i);
		switch (options[optindex(optv, i)].terse[0]) {
		case 's':
			opts.stats++;
			break;
		case 'b':
			opts.dump_block = value;
			break;
		case '?':
			usage(options, progname, command, blurb, " [OPTIONS]");
			exit(0);
		case 0:
			usage(options, progname, command, blurb, NULL);
			exit(0);
		}
	}

	sb_dev(sb)->fd = open_volume(volname);

	err = dump_main(sb, &opts);

	free(argv2optv(args));
	return err;
}

static int cmd_image(struct sb *sb, const char *progname, const char *command,
		     int argc, const char **args)
{
	struct options options[] = {
		{ "need_data", "d", 0, "Include data blocks too", },
		{ "verbose", "v", OPT_MANY, "dump level", },
		{ "usage", "", 0, "Show usage", },
		{ "help", "?", 0, "Show help", },
		{},
	};
	struct image_opts opts = {};
	const char *volname;
	int err;

	const char *blurb = "<src> <dst>";
	int optc = common_parse(&argc, &args, options, 4, progname,
				command, blurb, &volname);

	void *optv = argv2optv(args);

	for (int i = 0; i < optc; i++) {
		switch (options[optindex(optv, i)].terse[0]) {
		case 'd':
			opts.need_data = 1;
			break;
		case 'v':
			opts.verbose++;
			break;
		case '?':
			usage(options, progname, command, blurb, " [OPTIONS]");
			exit(0);
		case 0:
			usage(options, progname, command, blurb, NULL);
			exit(0);
		}
	}
	opts.dst_name = args[3];

	sb_dev(sb)->fd = open_volume(volname);

	err = image_main(sb, &opts);

	free(argv2optv(args));
	return err;
}

static int cmd_graph(struct sb *sb, const char *progname, const char *command,
		     int argc, const char **args)
{
	struct vars vars = {};
	int err;

	common_options(&argc, &args, onlyhelp, 3, progname, command,
		       "<volume>", &vars);

	sb_dev(sb)->fd = open_volume(vars.volname);

	err = graph_main(sb, vars.volname, vars.verbose);

	free(argv2optv(args));
	return err;
}

static void print_inode(struct inode *inode)
{
	struct tux3_inode *tuxnode = tux_inode(inode);

	printf("mode %07ho uid %x gid %x ",
	       inode->i_mode, i_uid_read(inode), i_gid_read(inode));
	printf("ctime %lld.%09ld size %Lx ",
	       inode->i_ctime.tv_sec, inode->i_ctime.tv_nsec,
	       (s64)inode->i_size);
	printf("mtime %lld.%09ld ",
	       inode->i_mtime.tv_sec, inode->i_mtime.tv_nsec);
	printf("links %u ", inode->i_nlink);
	printf("xattr(s) %p ", tuxnode->xcache);
	if (!has_no_root(&tuxnode->btree)) {
		printf("root %Lx:%u ",
		       tuxnode->btree.root.block, tuxnode->btree.root.depth);
	}
}

int main(int argc, char *argv[])
{
	const char *progname = optbasename(argv[0]);
	const char **args = (const char **)argv;
	const char *blurb = "<command> <volume>";

	enum {
		CMD_MKFS, CMD_FSCK, CMD_DUMP, CMD_IMAGE, CMD_GRAPH,

		CMD_DELTA, CMD_UNIFY,
		CMD_READ, CMD_WRITE, CMD_GET, CMD_SET, CMD_STAT, CMD_DELETE,
		CMD_TRUNCATE, CMD_LINK, CMD_SYMLINK, CMD_READLINK, CMD_UNKNOWN,
	};

	static char *commands[] = {
		[CMD_MKFS] = "mkfs", [CMD_FSCK] = "fsck", [CMD_DUMP] = "dump",
		[CMD_IMAGE] = "image", [CMD_GRAPH] = "graph",

		[CMD_DELTA] = "delta", [CMD_UNIFY] = "unify",
		[CMD_READ] = "read", [CMD_WRITE] = "write",
		[CMD_GET] = "get", [CMD_SET] = "set",
		[CMD_STAT] = "stat", [CMD_DELETE] = "delete",
		[CMD_TRUNCATE] = "truncate", [CMD_LINK] = "link",
		[CMD_SYMLINK] = "symlink", [CMD_READLINK] = "readlink",
	};

	struct options options[] = {
		{ "commands", "L", 0, "List commands", },
		{ "mount-option", "o", OPT_HASARG, "mount option", },
		{ "verbose", "v", OPT_MANY, "Verbose output", },
		{ "version", "V", 0, "Show version", },
		{ "usage", "", 0, "Show usage", },
		{ "help", "?", 0, "Show help", },
		{},
	};

	unsigned space = optspace(options, argc, args);
	void *optv = malloc(space);
	if (!optv)
		strerror_exit(1, errno, "malloc");

	/* 2 == require progname and command */
	int optc = opthead(options, &argc, &args, optv, space, 2);
	if (optc < 0)
		error_exit("%s!", opterror(optv));

	char *mount_option = NULL;
	int verbose = 0;

	for (int i = 0; i < optc; i++) {
		const char *value = optvalue(optv, i);
		switch (options[optindex(optv, i)].terse[0]) {
		case 'L':
			for (int j = 0; j < ARRAY_SIZE(commands); j++)
				printf("%s ", commands[j]);
			printf("\n");
			exit(0);
		case 'o':
			mount_option = strdup(value);
			if (mount_option == NULL)
				strerror_exit(1, errno, "strdup");
			break;
		case 'v':
			verbose++;
			break;
		case 'V':
			printf("Tux3 tools version %s\n", __stringify(VERSION));
			exit(0);
		case '?':
			usage(options, progname, NULL, blurb, " [OPTIONS]");
			exit(0);
		case 0:
			usage(options, progname, NULL, blurb, NULL);
			exit(0);
		}
	}

	/* At least, user has to specify "command" */
	if (argc < 2) {
		usage(options, progname, NULL, blurb, NULL);
		exit(1);
	}

	const char *command = args[1], *filename, *attrname;
	struct vars vars = { .blocksize = 1 << 12, .verbose = verbose };
	struct inode *inode = NULL;
	struct file *file = NULL;

	int err = tux3_init_mem(1 << 28, 2);
	if (err)
		goto error;

	struct dev *dev = &(struct dev){};
	struct sb *sb = rapid_sb(dev);	/* dev->bits still zero, take care */

	struct options onlyseek[] = {
		{ "seek", "s", OPT_NUMBER, "Set file position", },
		{ "verbose", "v", OPT_MANY, "Verbose output", },
		{ "usage", "", 0, "Show usage", },
		{ "help", "?", 0, "Show help", },
		{},
	};

	struct options onlysize[] = {
		{ "size", "s", OPT_NUMBER, "Specify file size", },
		{ "verbose", "v", OPT_MANY, "Verbose output", },
		{ "usage", "", 0, "Show usage", },
		{ "help", "?", 0, "Show help", },
		{},
	};

	int cmd;
	for (cmd = 0; cmd < ARRAY_SIZE(commands); cmd++) {
		if (commands[cmd] && !strcmp(command, commands[cmd]))
			break;
	}

	if (mount_option) {
		err = setup_mount_options(sb, mount_option);
		if (err)
			goto error;

		free(mount_option);
	}
	if (verbose) {
		char buf[4096];
		ssize_t len;
		len = get_mount_options(sb, buf, sizeof(buf), verbose >= 2);
		if (len < 0) {
			err = len;
			goto error;
		}
		printf("mount options: %s\n", buf);
	}

	switch (cmd) {
	case CMD_MKFS:
		err = cmd_mkfs(sb, progname, command, argc, args);
		if (err)
			goto error;
		break;

	case CMD_FSCK:
		err = cmd_fsck(sb, progname, command, argc, args);
		if (err)
			goto error;
		break;

	case CMD_DUMP:
		err = cmd_dump(sb, progname, command, argc, args);
		if (err)
			goto error;
		break;

	case CMD_IMAGE:
		err = cmd_image(sb, progname, command, argc, args);
		if (err)
			goto error;
		break;

	case CMD_GRAPH:
		err = cmd_graph(sb, progname, command, argc, args);
		if (err)
			goto error;
		break;

	case CMD_DELTA:
		common_options(&argc, &args, onlyhelp, 3, progname, command,
			       "<volume>", &vars);
		err = open_fs(vars.volname, sb);
		if (err)
			goto error;
		force_delta(sb);
		free(argv2optv(args));
		break;

	case CMD_UNIFY:
		common_options(&argc, &args, onlyhelp, 3, progname, command,
			       "<volume>", &vars);
		err = open_fs(vars.volname, sb);
		if (err)
			goto error;
		force_unify(sb);
		free(argv2optv(args));
		break;

	case CMD_WRITE: {
		common_options(&argc, &args, onlyseek, 4, progname, command,
			       "<volume> <filename>", &vars);
		filename = args[3];
		err = open_fs(vars.volname, sb);
		if (err)
			goto error;
		inode = tuxopen(sb->rootdir, filename, strlen(filename));
		if (IS_ERR(inode) && PTR_ERR(inode) == -ENOENT) {
			struct tux_iattr iattr = { .mode = S_IFREG | S_IRWXU, };
			inode = tuxcreate(sb->rootdir, filename, strlen(filename),
					  &iattr);
		}
		if (IS_ERR(inode)) {
			err = PTR_ERR(inode);
			goto error;
		}
		file = &(struct file)FILE_INIT(inode, 0);
		struct stat stat;
		if ((fstat(0, &stat)) == -1)
			strerror_exit(1, errno, "fstat");
		if (vars.seek)
			tuxseek(file, vars.seek);
		char text[1 << 16];
		while (1) {
			ssize_t len = read(0, text, sizeof(text));
			if (len < 0)
				strerror_exit(1, errno, "read");
			if (!len)
				break;
			len = tuxwrite(file, text, len);
			if (len < 0) {
				err = len;
				goto error;
			}
		}
		iput(inode);

		err = sync_super(sb);
		if (err)
			goto error;
		free(argv2optv(args));
		//bitmap_dump(sb->bitmap, 0, sb->volblocks);
		//tux_dump_entries(blockget(sb->rootdir->map, 0));
		break;
	}
	case CMD_READ: {
		common_options(&argc, &args, onlyseek, 4, progname, command,
			       "<volume> <filename>", &vars);
		filename = args[3];
		err = open_fs(vars.volname, sb);
		if (err)
			goto error;
		//tux_dump_entries(blockread(sb->rootdir->map, 0));
		inode = tuxopen(sb->rootdir, filename, strlen(filename));
		if (IS_ERR(inode)) {
			err = PTR_ERR(inode);
			goto error;
		}
		file = &(struct file)FILE_INIT(inode, 0);
		char buf[100];
		memset(buf, 0, sizeof(buf));
		if (vars.seek)
			tuxseek(file, vars.seek);
		int got = tuxread(file, buf, sizeof(buf));
		//printf("got %x bytes\n", got);
		iput(inode);
		if (got < 0) {
			err = got;
			goto error;
		}
		hexdump(buf, got);
		free(argv2optv(args));
		break;
	}
	case CMD_SET: {
		common_options(&argc, &args, onlyhelp, 5, progname, command,
			       "<volume> <filename> <attribute>", &vars);
		filename = args[3];
		attrname = args[4];
		err = open_fs(vars.volname, sb);
		if (err)
			goto error;
		inode = tuxopen(sb->rootdir, filename, strlen(filename));
		if (IS_ERR(inode)) {
			err = PTR_ERR(inode);
			goto error;
		}
		char text[1 << 16];
		ssize_t len;
		len = read(0, text, sizeof(text));
		if (len < 0)
			strerror_exit(1, errno, "read");
		if (verbose)
			printf("got %zd bytes\n", len);
		err = set_xattr(inode, attrname, strlen(attrname), text, len, 0);
		iput(inode);
		if (err)
			goto error;

		err = sync_super(sb);
		if (err)
			goto error;
		free(argv2optv(args));
		break;
	}
	case CMD_GET: {
		common_options(&argc, &args, onlyhelp, 5, progname, command,
			       "<volume> <filename> <attribute>", &vars);
		filename = args[3];
		attrname = args[4];
		err = open_fs(vars.volname, sb);
		if (err)
			goto error;
		inode = tuxopen(sb->rootdir, filename, strlen(filename));
		if (IS_ERR(inode)) {
			err = PTR_ERR(inode);
			goto error;
		}
		int size = get_xattr(inode, attrname, strlen(attrname), NULL, 0);
		if (size < 0) {
			err = size;
			goto error;
		}
		void *data = malloc(size);
		if (!data) {
			err = -ENOMEM;
			goto error;
		}
		size = get_xattr(inode, attrname, strlen(attrname), data, size);
		if (size < 0) {
			free(data);
			err = size;
			goto error;
		}
		hexdump(data, size);
		free(data);
		iput(inode);
		free(argv2optv(args));
		break;
	}
	case CMD_STAT: {
		common_options(&argc, &args, onlyhelp, 4, progname, command,
			       "<volume> <filename>", &vars);
		filename = args[3];
		err = open_fs(vars.volname, sb);
		if (err)
			goto error;
		inode = tuxopen(sb->rootdir, filename, strlen(filename));
		if (IS_ERR(inode)) {
			err = PTR_ERR(inode);
			goto error;
		}
		print_inode(inode);
		iput(inode);
		free(argv2optv(args));
		break;
	}
	case CMD_DELETE: {
		common_options(&argc, &args, onlyhelp, 4, progname, command,
			       "<volume> <filename>", &vars);
		filename = args[3];
		err = open_fs(vars.volname, sb);
		if (err)
			goto error;
		err = tuxunlink(sb->rootdir, filename, strlen(filename));
		if (err) {
			if (err == -ENOENT)
				printf("File not found\n");
			else
				goto error;
		}
		tux_dump_entries(blockread(sb->rootdir->map, 0));

		err = sync_super(sb);
		if (err)
			goto error;
		free(argv2optv(args));
		break;
	}
	case CMD_TRUNCATE: {
		common_options(&argc, &args, onlysize, 4, progname, command,
			       "<volume> <filename>", &vars);
		filename = args[3];
		err = open_fs(vars.volname, sb);
		if (err)
			goto error;
		inode = tuxopen(sb->rootdir, filename, strlen(filename));
		if (IS_ERR(inode)) {
			err = PTR_ERR(inode);
			goto error;
		}
		err = tuxtruncate(inode, vars.seek);
		iput(inode);
		if (err)
			goto error;

		err = sync_super(sb);
		if (err)
			goto error;
		free(argv2optv(args));
		break;
	}
	case CMD_LINK: {
		common_options(&argc, &args, onlysize, 5, progname, command,
			       "<volume> <path1> <path2>", &vars);
		filename = args[3];
		const char *dst = args[4];
		err = open_fs(vars.volname, sb);
		if (err)
			goto error;
		err = tuxlink(sb->rootdir, filename, strlen(filename),
			      dst, strlen(dst));
		if (err)
			goto error;

		err = sync_super(sb);
		if (err)
			goto error;
		free(argv2optv(args));
		break;
	}
	case CMD_SYMLINK: {
		common_options(&argc, &args, onlysize, 5, progname, command,
			       "<volume> <target> <filename>", &vars);
		const char *target = args[3];
		filename = args[4];
		err = open_fs(vars.volname, sb);
		if (err)
			goto error;
		struct tux_iattr iattr = {
			.uid = GLOBAL_ROOT_UID,
			.gid = GLOBAL_ROOT_GID,
		};
		err = tuxsymlink(sb->rootdir, filename, strlen(filename),
				 &iattr, target);
		if (err)
			goto error;

		err = sync_super(sb);
		if (err)
			goto error;
		free(argv2optv(args));
		break;
	}
	case CMD_READLINK: {
		common_options(&argc, &args, onlysize, 4, progname, command,
			       "<volume> <filename>", &vars);
		filename = args[3];
		err = open_fs(vars.volname, sb);
		if (err)
			goto error;
		char buf[4096];
		int len = tuxreadlink(sb->rootdir, filename, strlen(filename),
				      buf, sizeof(buf) - 1);
		if (len < 0)
			goto error;
		buf[len] = '\0';

		printf("%s: len %d, %s\n", filename, len, buf);

		free(argv2optv(args));
		break;
	}
	default:
		error_exit("'%s' is not a command", command);
	}

	//printf("---- show state ----\n");
	//show_buffers(sb->rootdir->map);
	//show_buffers(sb->volmap->map);
	put_super(sb);
	tux3_exit_mem();
	free(optv);
	return 0;

error:
	strerror_exit(1, -err, "eek!");
}
