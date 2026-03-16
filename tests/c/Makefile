CC      := clang
PYTHON  := python3

PY_INCLUDES  := $(shell $(PYTHON) -c \
    "import sysconfig; print('-I' + sysconfig.get_config_var('INCLUDEPY'))")
PY_FRAMEWORK := $(shell $(PYTHON) -c \
    "import sysconfig, os; \
     libdir = sysconfig.get_config_var('LIBDIR'); \
     print(os.path.abspath(os.path.join(libdir, '..', '..', '..', '..')))")

CFLAGS  := -std=c11 -Wall -Wextra -g \
           $(PY_INCLUDES) \
           -Wno-unused-function -Wno-unused-parameter \
           -Wno-missing-field-initializers \
           -Wno-cast-function-type-mismatch

LDFLAGS := -F$(PY_FRAMEWORK) -framework Python -pthread

TESTS   := test_shared_value

.PHONY: all run clean

all: $(TESTS)

test_shared_value: test_shared_value.c unity.c
	$(CC) $(CFLAGS) $^ -o $@ $(LDFLAGS)

run: all
	@for t in $(TESTS); do ./$$t; done

clean:
	rm -f $(TESTS)
