vpath %.c ../src

CC = gcc
CFLAGS = -c -Wall -std=gnu99
LDFLAGS =

DSTDIR := /usr/local
OBJDIR := obj
SRCDIR := src

SRC = clevo-indicator.c sni.c ec-monitor.c
OBJ = $(patsubst %.c,$(OBJDIR)/%.o,$(SRC)) 

TARGET = bin/clevo-indicator

CFLAGS += `pkg-config --cflags gtk+-3.0 ayatana-appindicator3-0.1` -DGDK_DISABLE_DEPRECATION_WARNINGS -DGTK_DISABLE_DEPRECATION_WARNINGS
LDFLAGS += `pkg-config --libs gtk+-3.0 ayatana-appindicator3-0.1` -lpthread

all: $(TARGET)

install: $(TARGET)
	@echo Install to ${DSTDIR}/bin/
	@sudo install -m 4755 $(TARGET) ${DSTDIR}/bin/
	@sudo chown root:root ${DSTDIR}/$(TARGET)
	@sudo chmod u+s ${DSTDIR}/$(TARGET)

test: $(TARGET)
	@sudo chown root $(TARGET)
	@sudo chgrp adm  $(TARGET)
	@sudo chmod 4750 $(TARGET)

$(TARGET): $(OBJ) Makefile
	@mkdir -p bin
	@echo linking $(TARGET) from $(OBJ)
	@$(CC) $(OBJ) -o $(TARGET) $(LDFLAGS) -lm

clean:
	rm -f $(OBJ) $(TARGET)

$(OBJDIR)/%.o : $(SRCDIR)/%.c Makefile
	@echo compiling $< 
	@mkdir -p obj
	@$(CC) $(CFLAGS) -c $< -o $@

$(OBJDIR)/clevo-indicator.o $(OBJDIR)/ec-monitor.o: src/ec-monitor.h

# Hardware-free tests. The historical "test" target above only sets permissions.
.PHONY: check
TEST_FLAGS = -Wall -Wextra -Werror -std=gnu99 -g $(TEST_SANITIZERS)
GPU_FIXTURES = bin/test-gpu-good bin/test-gpu-hang bin/test-gpu-invalid bin/test-gpu-error bin/test-gpu-overflow
check: bin/test-ec-monitor bin/test-worker $(GPU_FIXTURES)
	./bin/test-ec-monitor
	./bin/test-worker

bin/test-ec-monitor: tests/test-ec-monitor.c src/ec-monitor.c src/ec-monitor.h Makefile
	@mkdir -p bin
	$(CC) $(TEST_FLAGS) tests/test-ec-monitor.c src/ec-monitor.c -o $@

bin/test-worker: tests/test-worker.c src/clevo-indicator.c src/ec-monitor.c src/ec-monitor.h src/sni.c src/sni.h Makefile
	@mkdir -p bin
	$(CC) $(TEST_FLAGS) -Wno-unused-parameter -Wno-sign-compare -Wno-missing-field-initializers $(filter-out -c,$(CFLAGS)) tests/test-worker.c src/ec-monitor.c src/sni.c -o $@ $(LDFLAGS) -lm

bin/test-gpu-good: tests/fake-gpu.c
	@mkdir -p bin
	$(CC) -Wall -Wextra -Werror $< -o $@
bin/test-gpu-hang: tests/fake-gpu.c
	@mkdir -p bin
	$(CC) -Wall -Wextra -Werror -DGPU_MODE=1 $< -o $@
bin/test-gpu-invalid: tests/fake-gpu.c
	@mkdir -p bin
	$(CC) -Wall -Wextra -Werror -DGPU_MODE=2 $< -o $@
bin/test-gpu-error: tests/fake-gpu.c
	@mkdir -p bin
	$(CC) -Wall -Wextra -Werror -DGPU_MODE=3 $< -o $@
bin/test-gpu-overflow: tests/fake-gpu.c
	@mkdir -p bin
	$(CC) -Wall -Wextra -Werror -DGPU_MODE=4 $< -o $@

#$(OBJECTS): | obj

#obj:
#	@mkdir -p $@
