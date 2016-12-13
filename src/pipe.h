
#ifndef __PIPE_H__
#define __PIPE_H__

#include <event2/buffer.h>

int
pipe_setup(const char *path);

void
pipe_cleanup(void);

int
pipe_audio_get(struct evbuffer *evbuf, int wanted);

int
pipewatcher_init();

void
pipewatcher_deinit(void);

#endif /* !__PIPE_H__ */
