TOPDIR = $(CURDIR)

ARCH	= $(shell uname -m)

CC	= gcc
AR	= ar

# Target name for per target flags
basetarget = $(basename $@)

ifeq ($(ARCH),x86_64)
CFLAGS	+= -m64
endif
ifeq ($(ARCH),i686)
CFLAGS	+= -m32
endif

CFLAGS	+= -std=gnu99 -Wall -g -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
CFLAGS	+= -I$(TOPDIR) -DTUX3_BUILD=1
# gcc warning options
CFLAGS	+= -Wall -Wextra -Werror
CFLAGS	+= -Wundef -Wstrict-prototypes -Werror-implicit-function-declaration
CFLAGS	+= -Wno-unused-parameter -Wno-sign-compare
CFLAGS	+= -Wno-missing-field-initializers
#CFLAGS += -Wp,-Wunused-macros

# Debugging
#
# backtrace() would need this to see all symbols
CFLAGS	+= -rdynamic
# buffer debug
CFLAGS	+= -DBUFFER_PARANOIA_DEBUG=1
# cursor debug (this is defined in tux3.h)
#CFLAGS	+= -DCURSOR_DEBUG=1
# use LOCK_DEBUG
CFLAGS	+= -DLOCK_DEBUG=1
# use SMALL_PAGE_SIZE_DEBUG
CFLAGS	+= -DSMALL_PAGE_SIZE_DEBUG=1
# alignment checker
#ALIGN_CHECK = 1
ifeq ($(ALIGN_CHECK),1)
CFLAGS	+= -DALIGN_CHECK=1
LIBS += -ldl
# prevent unaligned error
NEED_MEMMOVE_UNALIGNED = 1
endif
# use refcount_t debugging
CFLAGS	+= -DCONFIG_REFCOUNT_FULL=1

# gcc checker (_FORTIFY_SOURCE requires optimize option >= 1)
# Rethink about this when optimization become default
#CFLAGS	+= -O1 -D_FORTIFY_SOURCE=2
# gcc sanitizers
ifeq ($(ASAN),1)
CFLAGS	+= -fsanitize=address -fsanitize=leak
endif
ifeq ($(UBSAN),1)
CFLAGS	+= -fsanitize=undefined
# prevent unaligned error
NEED_MEMMOVE_UNALIGNED = 1
endif
#CFLAGS += -fsanitize=thread

ifeq ($(NEED_MEMMOVE_UNALIGNED),1)
# To use memmove() for unaligned access
CFLAGS	+= -DCONFIG_MEMMOVE_UNALIGNED_ACCESS=1
endif

# gcov
ifneq ($(GCOV),)
CFLAGS	+= --coverage
endif
# userland can't use async, so use TUX3_FLUSHER_SYNC
CFLAGS	+= -DTUX3_FLUSHER_SYNC
# Per target CFLAGS
CFLAGS	+= $(CFLAGS_$(basetarget).o)
# user flags
CFLAGS	+= $(UCFLAGS)

# Per target LDFLAGS/LIBS
LDFLAGS	+= $(LDFLAGS_$@)
LIBS	+= $(LIBS_$@)

AFLAGS	= rcs

CHECKER	   = sparse
CHECKFLAGS = -gcc-base-dir $(shell $(CC) -print-file-name=)
# Add multiarch include if need (workaround for debian's multiarch)
CHECKFLAGS += $(shell if gcc -print-multiarch > /dev/null 2>&1; then \
		echo "-isystem /usr/include/`gcc -print-multiarch`"; fi)
CHECKFLAGS += -D__CHECKER__
CHECKFLAGS += -Wsparse-all -Wno-transparent-union
CHECKFLAGS += -Wno-declaration-after-statement
CHECKFLAGS += -Wno-decl
CHECKFLAGS += -Wno-ptr-subtraction-blows

INSTALL	= install

TESTDIR	= $(TOPDIR)
OWNER	= root
GROUP	= root

# check lcov option
lcov-opt-if = $(shell						\
	if lcov --help 2> /dev/null | grep -q -- "$(1)"; then	\
		echo "$(2)";					\
	else							\
		echo "$(3)";					\
	fi)
# lcov flags
TUX3_DIR = $(abspath $(TOPDIR)/../)
GCOV_DIR = gcov
LCOV_FLAGS = --capture
GENHTML_FLAGS = --prefix $(TUX3_DIR) --show-details --legend
# lcov-1.10 or above, branch coverage is off by default, enable it
LCOV_FLAGS += $(call lcov-opt-if,--rc,--rc lcov_branch_coverage=1)
GENHTML_FLAGS += $(call lcov-opt-if,--rc,--branch-coverage)

DISTDIR	=
PREFIX	= /usr/local
SBINDIR	= $(PREFIX)/sbin
LIBEXECDIR = $(PREFIX)/libexec/tux3

# binaries
TUX3_BIN	= tux3
ifeq ($(shell pkg-config fuse && echo found), found)
	FUSE_BIN = tux3fuse
	CFLAGS_tux3fuse.o = $(shell pkg-config --cflags fuse)
	LIBS_tux3fuse = -lfuse
endif
TEST_BIN	= tests/balloc tests/bdict_fixed tests/btree tests/buffer \
	tests/commit tests/dir tests/dleaf tests/fault tests/filemap \
	tests/iattr tests/ileaf tests/inode tests/log tests/xattr
ALL_BIN		= $(TEST_BIN) $(TUX3_BIN) $(FUSE_BIN)

# libraries
LIBTUX3		= libtux3.a
LIBKLIB		= libklib/libklib.a
ALL_LIBS	= $(LIBTUX3) $(LIBKLIB)

# LIBTUX3 objects
USER_OBJS	= commit.o current_task.o dir.o diskio.o filemap.o inode.o \
	namei.o options.o super.o utility.o writeback.o
ifeq ($(ALIGN_CHECK),1)
USER_OBJS	+= aligncheck.o
endif
KERN_OBJS	= ../balloc.o ../btree.o ../dleaf.o ../iattr.o ../ileaf.o \
	../log.o ../orphan.o ../policy.o ../replay.o ../xattr.o
TEST_LIB_OBJS	= tests/test.o

# LIBKLIB objects
LIBKLIB_OBJS	= libklib/find_bit.o libklib/fs.o libklib/list_sort.o \
	libklib/parser.o libklib/refcount.o libklib/seq_file.o libklib/slab.o \
	libklib/time.o libklib/uidgid.o

# binary objects
OBJS		= tux3.o
FUSE_OBJS	= tux3fuse.o
TEST_OBJS	= tests/balloc.o tests/bdict_fixed.o tests/btree.o \
	tests/buffer.o tests/commit.o tests/dir.o tests/dleaf.o \
	tests/fault.o tests/filemap.o tests/iattr.o tests/ileaf.o \
	tests/inode.o tests/log.o tests/xattr.o

# objects for common build rules
COMMON_OBJS	= $(USER_OBJS) $(TEST_LIB_OBJS) $(LIBKLIB_OBJS) $(OBJS) \
	$(TEST_OBJS) $(FUSE_OBJS)
# all objects
ALL_OBJS	= $(COMMON_OBJS) $(KERN_OBJS)
# directories includes objects
OBJS_DIRS := $(sort $(dir $(ALL_OBJS)))

# Per target flags
LIBS_tests/fault = -ldl

.PHONY: tests
all: $(ALL_BIN)

# objects dependency
$(LIBTUX3): $(USER_OBJS) $(KERN_OBJS) $(TEST_LIB_OBJS)
$(LIBKLIB): $(LIBKLIB_OBJS)
tux3: tux3.o $(ALL_LIBS)
tux3fuse: $(FUSE_OBJS) $(ALL_LIBS)
tests/balloc: tests/balloc.o $(ALL_LIBS)
tests/bdict_fixed: tests/bdict_fixed.o $(ALL_LIBS)
tests/btree: tests/btree.o $(ALL_LIBS)
tests/buffer: tests/buffer.o $(ALL_LIBS)
tests/commit: tests/commit.o $(ALL_LIBS)
tests/dir: tests/dir.o $(ALL_LIBS)
tests/dleaf: tests/dleaf.o $(ALL_LIBS)
tests/fault: tests/fault.o $(ALL_LIBS)
tests/filemap: tests/filemap.o $(ALL_LIBS)
tests/iattr: tests/iattr.o $(ALL_LIBS)
tests/ileaf: tests/ileaf.o $(ALL_LIBS)
tests/inode: tests/inode.o $(ALL_LIBS)
tests/log: tests/log.o $(ALL_LIBS)
tests/xattr: tests/xattr.o $(ALL_LIBS)

# dependency generation
DEPDIR	  := .deps
DEP_FILES := $(foreach f,$(ALL_OBJS),$(dir $f)$(DEPDIR)/$(notdir $f).d)
DEP_DIRS  := $(addsuffix $(DEPDIR),$(OBJS_DIRS))
MISSING_DEP_DIRS := $(filter-out $(wildcard $(DEP_DIRS)),$(DEP_DIRS))

$(DEP_DIRS):
	$(Q)mkdir -p $@

DEP_FILE = $(dir $@)$(DEPDIR)/$(notdir $@).d
DEP_ARGS = -MF $(DEP_FILE) -MP -MMD
# Take advantage of gcc's on-the-fly dependency generation
# See <http://gcc.gnu.org/gcc-3.0/features.html>.
DEP_FILES_PRESENT := $(wildcard $(DEP_FILES))
-include $(DEP_FILES_PRESENT)

# verbose or quiet
ifeq ($(V),1)
Q =
cmd =	$(2)
else
Q =	@
cmd =	@echo "  "$(1); $(2)
endif
export Q

# run checker or nothing
C ?= $(CHECK)
ifeq ($(C),1)
chk =	$(call cmd,$(1),$(2))
else
chk =
endif

# common build rules
$(COMMON_OBJS): %.o: %.c $(MISSING_DEP_DIRS)
	$(call cmd,"CC        $@",\
		$(CC) $(DEP_ARGS) $(CFLAGS) -c -o $@ $<)
	$(call chk,"CHECK     $<",\
		$(CHECKER) $(CHECKFLAGS) $(CFLAGS) -c $<)

# use -include tux3user.h to compile ../* without any change
$(KERN_OBJS): %.o: %.c $(MISSING_DEP_DIRS)
	$(call cmd,"CC        $@",\
		$(CC) $(DEP_ARGS) $(CFLAGS) -include tux3user.h -c -o $@ $<)
	$(call chk,"CHECK     $<",\
		$(CHECKER) $(CHECKFLAGS) $(CFLAGS) -include tux3user.h -c $<)

# build libraries rules
$(LIBTUX3) $(LIBKLIB):
	$(call cmd,"AR        $@",\
		rm -f $@ && $(AR) $(AFLAGS) $@ $^)

# build binaries rules
$(TEST_BIN) $(TUX3_BIN) $(FUSE_BIN):
	$(call cmd,"LINK      $@",\
		$(CC) $(CFLAGS) $(LDFLAGS) $^ $(LIBS) -o $@)

clean:
	$(call cmd,"CLEAN")
	$(Q)rm -f $(ALL_BIN) $(ALL_LIBS) $(ALL_OBJS)
	$(Q)rm -f a.out $(TESTDIR)/testdev
	$(Q)rm -f $(addsuffix *.gcda,$(OBJS_DIRS))
	$(Q)rm -f $(addsuffix *.gcno,$(OBJS_DIRS))
	$(Q)$(MAKE) -C tests clean

distclean: clean
	$(call cmd,"DISTCLEAN")
	$(Q)rm -f $(addsuffix *.orig,$(OBJS_DIRS))
	$(Q)rm -rf $(DEP_DIRS)
	$(Q)rm -rf $(GCOV_DIR)

install: install-bin install-test

install-bin: $(TUX3_BIN) $(FUSE_BIN)
	$(INSTALL) -c -o $(OWNER) -g $(GROUP) -m 755 -d $(DISTDIR)$(SBINDIR)
	$(INSTALL) -c -o $(OWNER) -g $(GROUP) -m 755 $(TUX3_BIN) $(FUSE_BIN) $(DISTDIR)$(SBINDIR)

install-test: install-bin $(TEST_BIN)
	$(INSTALL) -c -o $(OWNER) -g $(GROUP) -m 755 -d $(DISTDIR)$(LIBEXECDIR)
	$(INSTALL) -c -o $(OWNER) -g $(GROUP) -m 755 $(TEST_BIN) $(DISTDIR)$(LIBEXECDIR)

# tests rules
tests: $(TEST_BIN)
	$(MAKE) -C tests

# Use lcov to summarize gcov data
coverage:
	# NOTE: need to compile with GCOV=1
	[ -d $(GCOV_DIR) ] || mkdir $(GCOV_DIR)
	lcov $(LCOV_FLAGS) -b $(TOPDIR) -d $(TUX3_DIR) -o $(GCOV_DIR)/tux3.info
	genhtml $(GENHTML_FLAGS) -o $(GCOV_DIR) $(GCOV_DIR)/tux3.info

makefs mkfs: tux3
	dd if=/dev/zero of=$(TESTDIR)/testdev bs=1 count=1 seek=1M
	./tux3 mkfs $(TESTDIR)/testdev
	mkdir -p $(TESTDIR)/test
	if [[ ! -f /etc/fuse.conf ]]; then sudo sh -c "echo user_allow_other >/etc/fuse.conf"; fi

testfs testfuse: makefs
	./tux3fuse $(TESTDIR)/testdev $(TESTDIR)/test -o allow_other

debug defuse: tux3fuse
	sudo ./tux3fuse $(TESTDIR)/testdev $(TESTDIR)/test -o allow_other -f

untest:
	sudo umount $(TESTDIR)/test || true
	rmdir $(TESTDIR)/test

unbork:
	sudo umount -l $(TESTDIR)/test
