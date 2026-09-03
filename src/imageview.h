#ifndef IMAGEVIEW_H
#define IMAGEVIEW_H

/* Open the `at`th image of the session at full size, stepping through the
   others from there. 0 when there is no such image. */
int imageview_open(int at);

/* The image under a clicked screen cell, opened; 0 when nothing of an image
   is drawn there. */
int imageview_click(int row, int col);

#endif
