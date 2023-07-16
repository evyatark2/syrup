#CC=clang
SANITIZE=#-fsanitize=undefined -fsanitize=leak -fsanitize=address
CFLAGS=-g -Wall -pedantic -I/home/evyatar/.local/include `pkg-config --cflags mariadb libevent_pthreads libargon2 expat lua json-c` $(SANITIZE)
LDFLAGS=-L/home/evyatar/.local/lib -Wl,-rpath /home/evyatar/.local/lib $(SANITIZE)
LDLIBS=`pkg-config --libs mariadb libevent_pthreads libargon2 expat lua json-c` -lcmph

OBJDIR=obj

DEPDIR := .deps
DEPFLAGS=-MT $@ -MMD -MP -MF 
COMMON_SRCS=writer.c reader.c database.c crypt.c packet.c account.c wz.c character.c constants.c hash-map.c

CHANNEL_SRCS=$(COMMON_SRCS) channel/server.c channel/main.c channel/client.c channel/map.c channel/drops.c channel/config.c channel/scripting/client.c channel/scripting/job.c channel/scripting/event.c channel/scripting/events.c channel/scripting/reactor-manager.c channel/scripting/script-manager.c channel/shop.c channel/events.c party.c channel/thread-coordinator.c
CHANNEL_OBJS=$(CHANNEL_SRCS:%.c=$(OBJDIR)/%.o)

LOGIN_SRCS=$(COMMON_SRCS) login/server.c login/main.c login/handlers.c login/config.c
LOGIN_OBJS=$(LOGIN_SRCS:%.c=$(OBJDIR)/%.o)
	
all: login/login channel/channel Makefile

login/login: $(LOGIN_OBJS) | login
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

login: ; @mkdir -p $@

channel/channel: $(CHANNEL_OBJS) | channel
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

channel: ; @mkdir -p $@

$(OBJDIR)/%.o: src/%.c
$(OBJDIR)/%.o: src/%.c $(DEPDIR)/%.d | $(DEPDIR) $(OBJDIR)
	$(CC) $(DEPFLAGS) $(DEPDIR)/$*.d $(CFLAGS) -c -o $@ $<

$(OBJDIR)/login/%.o: src/login/%.c
$(OBJDIR)/login/%.o: src/login/%.c $(DEPDIR)/login/%.d | $(DEPDIR)/login $(OBJDIR)/login
	$(CC) $(DEPFLAGS) $(DEPDIR)/login/$*.d $(CFLAGS) -c -o $@ $<

$(OBJDIR)/channel/%.o: src/channel/%.c
$(OBJDIR)/channel/%.o: src/channel/%.c $(DEPDIR)/channel/%.d | $(DEPDIR)/channel $(OBJDIR)/channel
	$(CC) $(DEPFLAGS) $(DEPDIR)/channel/$*.d $(CFLAGS) -c -o $@ $<

$(OBJDIR)/channel/scripting/%.o: src/channel/scripting/%.c
$(OBJDIR)/channel/scripting/%.o: src/channel/scripting/%.c $(DEPDIR)/channel/scripting/%.d | $(DEPDIR)/channel/scripting $(OBJDIR)/channel/scripting
	$(CC) $(DEPFLAGS) $(DEPDIR)/channel/scripting/$*.d $(CFLAGS) -c -o $@ $<

$(DEPDIR): ; @mkdir -p $@
$(DEPDIR)/login: ; @mkdir -p $@
$(DEPDIR)/channel: ; @mkdir -p $@
$(DEPDIR)/channel/scripting: ; @mkdir -p $@

$(OBJDIR): ; @mkdir -p $@
$(OBJDIR)/login: ; @mkdir -p $@
$(OBJDIR)/channel: ; @mkdir -p $@
$(OBJDIR)/channel/scripting: ; @mkdir -p $@

DEPFILES := $(LOGIN_SRCS:%.c=$(DEPDIR)/%.d) $(CHANNEL_SRCS:%.c=$(DEPDIR)/%.d)

$(DEPFILES):

include $(wildcard $(DEPFILES))

.PHONY: clean

clean:
	rm -rf login/login channel/channel $(LOGIN_OBJS) $(CHANNEL_OBJS) $(DEPDIR) $(OBJDIR)
