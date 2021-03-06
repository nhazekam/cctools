include ../../../../config.mk
include $(CCTOOLS_HOME)/rules.mk

# Python always uses 'so' for its modules (even on Darwin)
CCTOOLS_DYNAMIC_SUFFIX = so
# SWIG produces code that causes a lot of warnings, so use -w to turn those off.
LOCAL_CCFLAGS = -w -fPIC -DNDEBUG $(CCTOOLS_PYTHON3_CCFLAGS)
LOCAL_LINKAGE = $(CCTOOLS_PYTHON3_LDFLAGS) -lz

EXTERNAL_DEPENDENCIES = $(CCTOOLS_HOME)/work_queue/src/libwork_queue.a $(CCTOOLS_HOME)/dttools/src/libdttools.a
WQPYTHONSO = _work_queue.$(CCTOOLS_DYNAMIC_SUFFIX)
LIBRARIES = $(WQPYTHONSO) work_queue.py work_queue_async.py
OBJECTS = work_queue_wrap.o
TARGETS = $(LIBRARIES) work_queue_example.py  work_queue_async_example.py

all: $(TARGETS)

# The odd symlink in the following rule is necessary to overcome a problem
# in the framework search path emitted by the Python configuration on macOS.
work_queue_wrap.c work_queue.py: work_queue.i work_queue.binding.py
	@echo "SWIG work_queue.i (python)"
	@$(CCTOOLS_SWIG) -o work_queue_wrap.c -python -py3 -I$(CCTOOLS_HOME)/dttools/src -I$(CCTOOLS_HOME)/work_queue/src work_queue.i
	@cat work_queue.binding.py >> work_queue.py
	ln -sf /System/Library/Frameworks/Python.framework .

$(WQPYTHONSO): work_queue_wrap.o $(EXTERNAL_DEPENDENCIES)

work_queue.binding.py work_queue_async.py work_queue_example.py work_queue_async_example.py: %.py: ../python/%.py
	cp $< $@
	$(CCTOOLS_PYTHON3_2TO3) --nobackups --no-diffs --write $@

test:

clean:
	rm -f $(OBJECTS) $(TARGETS) Python.framework work_queue.binding.py work_queue_async.py work_queue_example.py  work_queue_async_example.py work_queue.py work_queue_wrap.c *.pyc

install: all
	mkdir -p $(CCTOOLS_PYTHON3_PATH)
	chmod 755 work_queue.py
	cp work_queue.py work_queue_async.py _work_queue.$(CCTOOLS_DYNAMIC_SUFFIX) $(CCTOOLS_PYTHON3_PATH)/
	mkdir -p $(CCTOOLS_INSTALL_DIR)/doc
	cp work_queue_example.py work_queue_async_example.py $(CCTOOLS_INSTALL_DIR)/doc/

