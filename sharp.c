#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/workqueue.h>
#include <linux/spi/spi.h>

#include <linux/kthread.h> 
#include <linux/sched.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/timer.h>

#include <linux/fb.h>
#include <linux/mm.h>
#include <linux/init.h>
#include <linux/vmalloc.h>

#include <linux/gpio.h>
#include <linux/uaccess.h>

#define LCDWIDTH 400
#define VIDEOMEMSIZE    (1*1024*1024)   /* 1 MB */
#define DEBUG_INT(x) printf( #x " at line %d; result: %d\n", __LINE__, x)

/*  Modifying to work with JDI LPM027M128C 8 color display 
    It is pin compatible with the sharp display. 
    Referencing JDI_MIP_Display.cpp for hints on
    what needs to be changed.*/

char commandByte    = 0b10000000;
// char vcomByte    = 0b01000000;
char clearByte      = 0b00100000;
char paddingByte    = 0b00000000;
char fourBitWriteByte  = 0b10010000;
char threeBitWriteByte  = 0b10000000;

 char DISP       = 22;
 char SCS        = 8;
 char VCOM       = 23;


int lcdWidth = LCDWIDTH;
int lcdHeight = 240;
int fpsCounter;

static int seuil = 4; // Indispensable pour fbcon
module_param(seuil, int, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP );

char vcomState;

// unsigned char lineBuffer[3*LCDWIDTH/8];

struct sharp {
    struct spi_device	*spi;
    int			id;
    char			name[sizeof("sharp-3")];

    struct mutex		mutex;
    struct work_struct	work;
    spinlock_t		lock;
};

struct sharp   *screen;
struct fb_info *info;
// struct vfb_data *vdata;

static void *videomemory;
static u_long videomemorysize = VIDEOMEMSIZE;

void vfb_fillrect(struct fb_info *p, const struct fb_fillrect *region);
// static int vfb_setcolreg(unsigned int regno, unsigned int red, unsigned int green, unsigned int blue, unsigned int transp, struct fb_info *info);
static int vfb_mmap(struct fb_info *info, struct vm_area_struct *vma);
void sendLine(char *buffer, char lineNumber);

static struct fb_var_screeninfo vfb_default = {
    .xres =     400,
    .yres =     240,
    .xres_virtual = 400,
    .yres_virtual = 240,
    .bits_per_pixel = 8,
    .grayscale = 0,
    .red =      { 1, 0, 0 },
    .green =    { 0, 1, 0 },
    .blue =     { 0, 0, 1 },
    .activate = FB_ACTIVATE_NOW,
    .height =   400,
    .width =    240,
    .pixclock = 20000,
    .left_margin =  0,
    .right_margin = 0,
    .upper_margin = 0,
    .lower_margin = 0,
    .hsync_len =    128,
    .vsync_len =    128,
    .vmode =    FB_VMODE_NONINTERLACED,
    };

static struct fb_fix_screeninfo vfb_fix = {
    .id =       "Sharp FB",
    .type =     FB_TYPE_PACKED_PIXELS,
    .line_length = 400,
    .xpanstep = 0,
    .ypanstep = 0,
    .ywrapstep =    0,
    .visual =	FB_VISUAL_PSEUDOCOLOR,
    // TODO: see if we can use hw acceleration at all for pi zero.
    .accel =    FB_ACCEL_NONE,
};

static struct fb_ops vfb_ops = {
    .fb_read        = fb_sys_read,
    .fb_write       = fb_sys_write,
    .fb_fillrect    = sys_fillrect,
    .fb_copyarea    = sys_copyarea,
    .fb_imageblit   = sys_imageblit,
    // .fb_image = sys_image,
    .fb_mmap    = vfb_mmap,
    // .fb_setcolreg   = vfb_setcolreg,
};

// struct vfb_data {
//     u32 palette[8]; // Array to store color palette entries
//     // Add other driver-specific data members as needed
// };

static struct task_struct *thread1;
static struct task_struct *fpsThread;
static struct task_struct *vcomToggleThread;

// static int vfb_setcolreg(unsigned int regno, unsigned int red, unsigned int green, unsigned int blue, unsigned int transp, struct fb_info *info)
// {
//     struct vfb_data *vdata = info->par; // Assuming you have a structure called vfb_data to hold your driver-specific data
    
//     if (regno >= 8)
//         return -EINVAL; // Invalid color palette index
    
//     // Assuming your display uses 3 bits per color component (bits_per_pixel = 3)
//     vdata->palette[regno] = ((red & 0x7) << 5) | ((green & 0x7) << 2) | (blue & 0x3);

//     return 0;
// }

static int vfb_mmap(struct fb_info *info, struct vm_area_struct *vma)
{
    unsigned long start = vma->vm_start;
    unsigned long size = vma->vm_end - vma->vm_start;
    unsigned long offset = vma->vm_pgoff << PAGE_SHIFT;
    unsigned long page, pos;
    printk(KERN_CRIT "start %ld size %ld offset %ld", start, size, offset);

    if (vma->vm_pgoff > (~0UL >> PAGE_SHIFT))
        return -EINVAL;
    if (size > info->fix.smem_len)
        return -EINVAL;
    if (offset > info->fix.smem_len - size)
        return -EINVAL;

    pos = (unsigned long)info->fix.smem_start + offset;

    while (size > 0) {
        page = vmalloc_to_pfn((void *)pos);
        if (remap_pfn_range(vma, start, page, PAGE_SIZE, PAGE_SHARED)) {
            return -EAGAIN;
        }
        start += PAGE_SIZE;
        pos += PAGE_SIZE;
        if (size > PAGE_SIZE)
            size -= PAGE_SIZE;
        else
            size = 0;
    }

    return 0;
}

void vfb_fillrect(struct fb_info *p, const struct fb_fillrect *region)
{
    printk(KERN_CRIT "from fillrect");
}

static void *rvmalloc(unsigned long size)
{
    void *mem;
    unsigned long adr;  /* Address of the allocated memory */

    size = PAGE_ALIGN(size);
    mem = vmalloc_32(size);
    if (!mem)
        return NULL;

    memset(mem, 0, size); /* Clear the ram out, no junk to the user */
    adr = (unsigned long) mem;
    while (size > 0) {
        SetPageReserved(vmalloc_to_page((void *)adr));
        adr += PAGE_SIZE;
        size -= PAGE_SIZE;
    }

    return mem;
}

static void rvfree(void *mem, unsigned long size)
{
    unsigned long adr;

    if (!mem)
        return;

    adr = (unsigned long) mem;
    while ((long) size > 0) {
        ClearPageReserved(vmalloc_to_page((void *)adr));
        adr += PAGE_SIZE;
        size -= PAGE_SIZE;
    }
    vfree(mem);
}

void clearDisplay(void) {
    char buffer[2] = {clearByte, paddingByte};
    gpio_set_value(SCS, 1);

    spi_write(screen->spi, (const u8 *)buffer, 2);

    gpio_set_value(SCS, 0);
}

char reverseByte(char b) {
    b = (b & 0xF0) >> 4 | (b & 0x0F) << 4;
    b = (b & 0xCC) >> 2 | (b & 0x33) << 2;
    b = (b & 0xAA) >> 1 | (b & 0x55) << 1;
    return b;
}

int vcomToggleFunction(void* v) 
{
    while (!kthread_should_stop()) 
    {
        msleep(50);
        vcomState = vcomState ? 0:1;
        gpio_set_value(VCOM, vcomState);
    }
    return 0;
}

int fpsThreadFunction(void* v)
{
    while (!kthread_should_stop()) 
    {
        msleep(5000);
        printk(KERN_DEBUG "FPS sharp : %d\n", fpsCounter);
        fpsCounter = 0;
    }
    return 0;
}

int thread_fn(void* v) 
{
    //BELOW, 50 becomes 150 becaues we have 3 bits (rgb) per pixel
    uint8_t x, y, i, byte_position;
    char r, g, b;
    uint8_t rByte, gByte, bByte;
    uint8_t rBit, gBit, bBit;
    char p;
    char c[3]; // reduced to 8 x 3 bit pixels as 3 bytes
    // int shift;
    // uint24_t c;
    char byteHasChanged, lineHasChanged = 0;

    unsigned char *screenBuffer;

    clearDisplay();

    screenBuffer = vzalloc((150+4)*240*sizeof(unsigned char)); 	//plante si on met moins

    // Init screen to black
    for(y=0 ; y < 240 ; y++)
    {
    gpio_set_value(SCS, 1);
    screenBuffer[y*(150+4)] = commandByte;
    screenBuffer[y*(150+4) + 1] = y; //reverseByte(y+1); //sharp display lines are indexed from 1
    screenBuffer[y*(150+4) + 152] = paddingByte;
    screenBuffer[y*(150+4) + 153] = paddingByte;

    //screenBuffer is all to 0 by default (vzalloc)
    spi_write(screen->spi, (const u8 *)(screenBuffer+(y*(150+4))), 154);
    gpio_set_value(SCS, 0);
    }


    while (!kthread_should_stop()) 
    {
        msleep(50);

        for(y=0 ; y < 240 ; y++)
        {
            lineHasChanged = 0;

            for(x=0 ; x<50 ; x++)
            {
                // We work on 8 pixels at a time... 50 * 8 => 400 pixels
                // Each 8 pixels compress indo 3 byte c[] and are copied to the screenBuffer.

                memset(c, 0, sizeof(c));
                byteHasChanged = 0;
                byte_position = 2 + x*3 + y*(150+4);

                // Iterate over 8 pixels
                for (i = 0; i < 8; i++) {
                    p = ioread8((void*)((uintptr_t)info->fix.smem_start + ((x+ 1)*8 + i + y*400)));

                    // Extract the red, green, and blue values for the current pixel
                    r = (p & 0x07) * 36;                // Bit 0-2 for red (0-7 scaled to 0-252)
                    g = ((p & 0x38) >> 3) * 36;         // Bit 3-5 for green (0-7 scaled to 0-252)
                    b = ((p & 0xC0) >> 6) * 85;         // Bit 6-7 for blue (0-3 scaled to 0-255)
                    
                    // convert to 1 bit
                    r = r >= 126 ? 1 : 0;
                    g = g >= 126 ? 1 : 0;
                    b = b >= 128 ? 1 : 0;
                    
                    // index bytes
                    rByte = (x*3)/8;
                    gByte = (x*3+1)/8;
                    bByte = (x*3+2)/8;
                    
                    //index bits
                    rBit = (x*3) % 8;
                    gBit = (x*3+1) % 8;
                    bBit = (x*3+2) % 8;
                    
                    // Pack the red, green, and blue bits into the c byte array
                    c[rByte] |= (r << (rBit));  // Pack red bit
                    c[gByte] |= (g << (gBit));  // Pack green bit
                    c[bByte] |= (b << (bBit));  // Pack blue bit   
                }

                // compare to screen buffer
                //if( screenBuffer[byte_position] != c[0] ||
                //    screenBuffer[byte_position + 1] != c[1] ||
                //    screenBuffer[byte_position + 2] != c[2])
                //{
                //    lineHasChanged = 1;
                //    byteHasChanged = 1; // !!! THIS WON'T WORK - NOT NECESSARILY CONTIGUOUS!!
                //}

                // update screen buffer
                //if (byteHasChanged)
                //{
                    // Test: this works (makes entire screen 'white')
                    //screenBuffer[2 + x * 3 + y*(150+4)] = 255;
                    //screenBuffer[2 + x * 3 + 1 + y*(150+4)] = 255;
                    //screenBuffer[2 + x * 3 + 2 + y*(150+4)] = 255;
                    // Test: this works (makes entire screen green)
                    //screenBuffer[2 + x * 3 + y*(150+4)] = 73;         0b01001001
                    //screenBuffer[2 + x * 3 + 1 + y*(150+4)] = 36;     0b00100100
                    //screenBuffer[2 + x * 3 + 2 + y*(150+4)] = 146;    0b10010010
                    
                    // So this must be working:
                    screenBuffer[byte_position] = c[0];
                    screenBuffer[byte_position + 1] = c[1];
                    screenBuffer[byte_position + 2] = c[2];
                    
                    
                //}
            }

            //if (lineHasChanged)
            //{
                gpio_set_value(SCS, 1);
                spi_write(screen->spi, (const u8 *)(screenBuffer+(y*(150+4))), 154);
                gpio_set_value(SCS, 0);
            //}
        }
    }

    return 0;
}

static int sharp_probe(struct spi_device *spi)
{
    char our_thread[] = "updateScreen";
    char thread_vcom[] = "vcom";
    char thread_fps[] = "fpsThread";
    int retval;

    screen = devm_kzalloc(&spi->dev, sizeof(*screen), GFP_KERNEL);
    if (!screen)
        return -ENOMEM;

    spi->bits_per_word  = 8;
    spi->max_speed_hz   = 2000000;

    screen->spi	= spi;

    spi_set_drvdata(spi, screen);

    thread1 = kthread_create(thread_fn,NULL,our_thread);
    if((thread1))
    {
        wake_up_process(thread1);
    }

    fpsThread = kthread_create(fpsThreadFunction,NULL,thread_fps);
    if((fpsThread))
    {
        wake_up_process(fpsThread);
    }

    vcomToggleThread = kthread_create(vcomToggleFunction,NULL,thread_vcom);
    if((vcomToggleThread))
    {
        wake_up_process(vcomToggleThread);
    }

    gpio_request(SCS, "SCS");
    gpio_direction_output(SCS, 0);

    gpio_request(VCOM, "VCOM");
    gpio_direction_output(VCOM, 0);

    gpio_request(DISP, "DISP");
    gpio_direction_output(DISP, 1);

    // SCREEN PART
    retval = -ENOMEM;

    if (!(videomemory = rvmalloc(videomemorysize)))
        return retval;

    memset(videomemory, 0, videomemorysize);

    info = framebuffer_alloc(sizeof(u32) * 256, &spi->dev);
    if (!info)
        goto err;

    info->screen_base = (char __iomem *)videomemory;
    info->fbops = &vfb_ops;

    info->var = vfb_default;
    vfb_fix.smem_start = (unsigned long) videomemory;
    vfb_fix.smem_len = videomemorysize;
    info->fix = vfb_fix;
    info->par = NULL;
    info->flags = FBINFO_FLAG_DEFAULT;

    retval = fb_alloc_cmap(&info->cmap, 16, 0);
    if (retval < 0)
        goto err1;

    retval = register_framebuffer(info);
    if (retval < 0)
        goto err2;

    fb_info(info, "Virtual frame buffer device, using %ldK of video memory\n",
        videomemorysize >> 10);
    return 0;
err2:
    fb_dealloc_cmap(&info->cmap);
err1:
    framebuffer_release(info);
err:
    rvfree(videomemory, videomemorysize);

    return 0;
}

static int sharp_remove(struct spi_device *spi)
{
        if (info) {
                unregister_framebuffer(info);
                fb_dealloc_cmap(&info->cmap);
                framebuffer_release(info);
        }
    kthread_stop(thread1);
    kthread_stop(fpsThread);
    kthread_stop(vcomToggleThread);
    printk(KERN_CRIT "out of screen module");
    return 0;
}

static struct spi_driver sharp_driver = {
    .probe          = sharp_probe,
    .remove         = sharp_remove,
    .driver = {
        .name	= "sharp",
        .owner	= THIS_MODULE,
    },
};

module_spi_driver(sharp_driver);

MODULE_AUTHOR("Ael Gain <ael.gain@free.fr>");
MODULE_DESCRIPTION("Sharp memory lcd driver");
MODULE_LICENSE("GPL v2");
MODULE_ALIAS("spi:sharp");
