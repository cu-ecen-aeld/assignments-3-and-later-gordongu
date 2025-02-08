SRC := writer.c
TARGET := writer
OBJS := $(SRC:.c=.o)

# value for compiling to aarch64
AARCH64_COMPILE := aarch64-none-linux-gnu-
# variable to assign target compiler 
CROSS_COMPILE :=

# check if target compiler was assigned
# otherwise compile natively
ifeq ($(CROSS_COMPILE), $(AARCH64_COMPILE))
	CC := $(AARCH64_COMPILE)gcc
else
	CC := cc
endif

all: $(TARGET)

$(OBJS): $(SRC)
	$(CC) -c $(SRC) -o $(OBJS)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) $(INCLUDES) $(OBJS) -o $(TARGET) $(LDFLAGS)

clean:
	-rm -f *.o $(TARGET) *.elf *.map