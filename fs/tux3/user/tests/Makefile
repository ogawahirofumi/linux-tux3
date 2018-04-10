ifeq ($(ASAN),1)
WITH_SANITIZER = 1
endif
ifeq ($(UBSAN),1)
WITH_SANITIZER = 1
endif

# valgrind and ASAN/UBSAN are incompatible, so disable valgrind for tests
ifneq ($(WITH_SANITIZER),1)
VG	?= valgrind $(VGFLAGS)
endif
VGFLAGS	+= --error-exitcode=200
VGFLAGS	+= --leak-check=full --show-leak-kinds=all --errors-for-leak-kinds=all

# disable color diagnostic messages if run under emacs
ifeq ($(TERM),dumb)
ASAN_OPTIONS = color=never
export ASAN_OPTIONS
UBSAN_OPTIONS = color=never
export UBSAN_OPTIONS
endif

# test rules
.NOTPARALLEL: tests
# The "tests" run in order, otherwise those will output the result mixed.

all: test_balloc test_btree test_buffer test_commit test_dir test_dleaf \
	test_dleaf test_fault test_filemap test_iattr test_ileaf test_inode \
	test_log test_xattr

clean:
	$(Q)rm -f foodev

test_balloc: balloc
	$(VG) ./balloc

test_btree: btree
	$(VG) ./btree foodev

test_buffer: buffer
	$(VG) ./buffer

test_commit: commit
	$(VG) ./commit foodev

test_dir: dir
	$(VG) ./dir

test_dleaf: dleaf
	$(VG) ./dleaf

test_fault: fault
# ASAN/UBSAN are incompatible for dlsym hook of malloc, etc.
ifneq ($(WITH_SANITIZER),1)
	$(VG) ./fault foodev
else
	@echo "skip fault test: incompatible with ASAN/UBSAN"
endif

test_filemap: filemap
	$(VG) ./filemap foodev

test_iattr: iattr
	$(VG) ./iattr

test_ileaf: ileaf
	$(VG) ./ileaf

test_inode: inode
	$(VG) ./inode foodev

test_log: log
	$(VG) ./log

test_xattr: xattr
	$(VG) ./xattr foodev
