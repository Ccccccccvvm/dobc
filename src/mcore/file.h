﻿

#ifndef __file_h__
#define __file_h__

#ifdef __cplusplus
extern "C" {
#endif

	int file_exist(char *filename);
	int file_save(const char *filename, char *buf, int len);
	char* file_load(const char *filename, int *len);
	int file_unload(void *data);

#ifdef __cplusplus
}
#endif

#endif