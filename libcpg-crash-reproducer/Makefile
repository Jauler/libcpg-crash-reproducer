TARGET=libcpg-crash-reproducer

CSRCS=main.c

CFLAGS+=-Wall

LDFLAGS+=-lcpg

DEPFLAGS+=-MT $@ -MMD -MP -MF $*.d

CC?=gcc

OBJS=$(CSRCS:.c=.o)
DEPS=$(CSRCS:.c=.d)


all: executable

executable: $(OBJS)
	$(CROSS)$(CC) $(OBJS) $(LDFLAGS) -o $(TARGET)

%.o : %.c %.d
	$(CROSS)$(CC) $(CFLAGS) $(DEPFLAGS) -c $< -o $@

%.o : %.s
	$(CROSS)$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS)
	rm -f $(DEPS)
	rm -f $(TARGET)

%.d: ;
.PRECIOUS: %.d

-include $(patsubst %,%.d,$(basename $(CSRCS)))



