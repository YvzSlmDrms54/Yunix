#ifndef APPS_H
#define APPS_H

/* Programs that live in yunix/apps/. Each one takes over the screen and
 * keyboard while it runs and gives them back to the shell when it ends. */

/* yvim <file> - a small vi-style editor. */
void yvim_run(const char *filename);

#endif
