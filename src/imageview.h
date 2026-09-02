#ifndef IMAGEVIEW_H
#define IMAGEVIEW_H

/* Open the `at`th image of the session at full size, stepping through the
   others from there. 0 when there is no such image. */
int imageview_open(int at);

/* The image under a clicked screen row, opened; 0 when the row holds none. */
int imageview_click(int row);

#endif
