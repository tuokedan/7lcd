#include <string.h>
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "screen_utility.h"
#include "zk.h"
#include "lcd.h"




extern void lcd_set_cs(uint8_t state);
extern void SetBitColor(u16 x,u16 y,u16 color);
void Display_Asc(u16 x,u16 y,u8 zk_num, u8 trans, u16 fc,u16 bc);
extern scr_handle_t g_lcd_handle;


#include "interface_drv_def.h"

void ZK_CS_Clr() //字库片选
{
    gpio_set_level(LCD_CS, 1);
}

void ZK_CS_Set()//取消字库片选
{
    gpio_set_level(LCD_CS, 0);
}

//字库芯片初始化
void zk_init()
{
    gpio_set_direction(LCD_CS, GPIO_MODE_OUTPUT);
}

u8 FontBuf[130];//字库缓存
/******************************************************************************
      函数说明：读取N个数据
      入口数据：AddrHigh  写地址高字节
                AddrMid   写地址中字节
                AddrLow   写地址低字节
                *pBuff    读取的数据
                DataLen   读取数据的长度
      返回值：  无
******************************************************************************/
void get_n_bytes_data_from_ROM(u8 AddrHigh,u8 AddrMid,u8 AddrLow,u8 *pBuff,u8 DataLen)
{
 	u8 i;
 	int ret =0;
 	static uint8_t data[5]={0};
 	ZK_CS_Clr(); //字库片选

  data[0]=0x03;
  data[1]=AddrHigh;
  data[2]=AddrMid;
  data[3]=AddrLow;
  ret = LCD_WRITE((uint8_t *)data, 4);
  LCD_READ(pBuff, DataLen);

	ZK_CS_Set();//取消字库片选
}


/******************************************************************************
      函数说明：显示汉字
      入口数据：x,y      写入的坐标
                zk_num   12:12*12,  16:16*16,  24:24*24,  32:32*32
                fc 字体颜色
                bc 背景颜色
      返回值：  无
******************************************************************************/
void Display_GB2312(u16 x,u16 y,u8 zk_num, u8 trans, u16 fc,u16 bc)
{
    u8 i=0,k=0,n=0,d=0,m=0;
    u16 clr=0;
    u8 offset=0;

	switch(zk_num)
	{
		// n:字符所占字节数  d：字间距
		case 12 :  n=24;  d=12; break;   // 12*12
		case 16 :  n=32;  d=16; break;   // 15*16
		case 24 :  n=72;  d=24; break;   // 24*24
		case 32 :  n=128; d=32; break;   // 32*32
		default:return;
	}

    for(i=0;i<n;i++)
	{
		for(k=0;k<8;k++)
		{
			if((FontBuf[i]&(0x80>>k)))
			{
			    clr=fc;
			}
			else
			{
			    if(trans>0) continue;
			    clr=bc;
			}

            if(d>8 && d<=16)
            {
                if((i%2)!=0)
                {
                    SetBitColor(x+k+8, y+i/2, clr);
                }
                else
                {
                    SetBitColor(x+k, y+i/2, clr);
                }
            }
            else if(d>16 && d<=24)
            {
                offset=i%3;
                if(offset==1)
                {
                    SetBitColor(x+k+8, y+i/3, clr);
                }
                else if(offset==2)
                {
                    SetBitColor(x+k+16, y+i/3, clr);
                }
                else
                {
                    SetBitColor(x+k, y+i/3, clr);
                }
            }
            else if(d>24 && d<=32)
            {
                offset=i%4;
                if(offset==1)
                {
                    SetBitColor(x+k+8, y+i/4, clr);
                }
                else if(offset==2)
                {
                    SetBitColor(x+k+16, y+i/4, clr);
                }
                else if(offset==3)
                {
                    SetBitColor(x+k+24, y+i/4, clr);
                }
                else
                {
                    SetBitColor(x+k, y+i/4, clr);
                }
            }
		}
	}
}


/******************************************************************************
      函数说明：显示汉字和ASCLL
      入口数据：zk_num    12:12*12,  16:15*16,  24:24*24,  32:32*32
                x,y       坐标
                text[]    要显示的汉字和英文组合
                fc 字体颜色
                bc 背景颜色
      返回值：  无
******************************************************************************/
void Display_GB2312_String(u16 x,u16 y,u8 zk_num, u8 trans, u8 text[],u16 fc,u16 bc)
{
	u8 i= 0;
	u8 AddrHigh=0,AddrMid=0,AddrLow=0; //字高、中、低地址
	u32 FontAddr=0; //字地址
	u32 BaseAdd=0; //字库基地址
	u8 n=0,d=0;// 不同点阵字库的计算变量

    zk_init();//先初始化

	while((text[i]>0x00))
	{
		if(((text[i]>=0xA1)&&(text[i]<=0xA9))&&(text[i+1]>=0xA1))
		{
            switch(zk_num)
            {
                // n:字符所占字节数  d：字间距
                case 12 :  BaseAdd=0x00;    n=24;  d=12; break;   // 12*12
                case 16 :  BaseAdd=0x2C9D0; n=32;  d=16; break;   // 15*16
                case 24 :  BaseAdd=0x68190; n=72;  d=24; break;   // 24*24
                case 32 :  BaseAdd=0xEDF00; n=128; d=32; break;   // 32*32
                default:x+=d;i+=2;continue;
            }


			//国标简体（GB2312）汉字在 字库IC中的地址由以下公式来计算：//
			//Address = ((MSB - 0xA1) * 94 + (LSB - 0xA1))*n+ BaseAdd; 分三部取地址///
			FontAddr = (text[i]- 0xA1)*94;
			FontAddr += (text[i+1]-0xA1);
			FontAddr = (unsigned long)((FontAddr*n)+BaseAdd);

			AddrHigh = (FontAddr&0xff0000)>>16;  //地址的高8位,共24位//
			AddrMid = (FontAddr&0xff00)>>8;      //地址的中8位,共24位//
			AddrLow = FontAddr&0xff;	     //地址的低8位,共24位//
			get_n_bytes_data_from_ROM(AddrHigh,AddrMid,AddrLow,FontBuf,n );//取一个汉字的数据，存到"FontBuf[]"
			Display_GB2312(x,y,zk_num,trans,fc,bc);//显示一个汉字到LCD上/

    		x+=d; //下一个字坐标
    		i+=2;  //下个字符
		}
		else if(((text[i]>=0xB0) &&(text[i]<=0xF7))&&(text[i+1]>=0xA1))
		{
        	switch(zk_num)
        	{
        		// n:字符所占字节数  d：字间距
        		case 12 :  BaseAdd=0x00;    n=24;  d=12; break;   // 12*12
        		case 16 :  BaseAdd=0x2C9D0; n=32;  d=16; break;   // 15*16
        		case 24 :  BaseAdd=0x68190; n=72;  d=24; break;   // 24*24
        		case 32 :  BaseAdd=0xEDF00; n=128; d=32; break;   // 32*32
        		default:x+=d;i+=2;continue;
        	}

			//国标简体（GB2312） 字库IC中的地址由以下公式来计算：//
			//Address = ((MSB - 0xB0) * 94 + (LSB - 0xA1)+846)*n+ BaseAdd; 分三部取地址//
			FontAddr = (text[i]- 0xB0)*94;
			FontAddr += (text[i+1]-0xA1)+846;
			FontAddr = (unsigned long)((FontAddr*n)+BaseAdd);

			AddrHigh = (FontAddr&0xff0000)>>16;  //地址的高8位,共24位//
			AddrMid = (FontAddr&0xff00)>>8;      //地址的中8位,共24位//
			AddrLow = FontAddr&0xff;	     //地址的低8位,共24位//
			get_n_bytes_data_from_ROM(AddrHigh,AddrMid,AddrLow,FontBuf,n );//取一个汉字的数据，存到"FontBuf[ ]"
			Display_GB2312(x,y,zk_num, trans, fc,bc);//显示一个汉字到LCD上/

            x+=d; //下一个字坐标
            i+=2;  //下个字符
		}
        else if((text[i] >= 0x20) &&(text[i] <= 0x7E))//ASCLL码
        {
        	switch(zk_num)
        	{
        		// n:字符所占字节数  d：字间距
                case 12:  BaseAdd=0x1DBE00; n=12; d=6;  break;   //  6x12 ASCII
                case 16:  BaseAdd=0x1DD780; n=16; d=8;  break;   //  8x16 ASCII
                case 24:  BaseAdd=0x1DFF00; n=48; d=12; break;   //  12x24 ASCII
                case 32:  BaseAdd=0x1E5A50; n=64; d=16; break;   //  16x32 ASCII
                default:x+=d;i+=1;continue;
        	}

            FontAddr =  text[i]-0x20;
            FontAddr = (unsigned long)((FontAddr*n)+BaseAdd);

            AddrHigh = (FontAddr&0xff0000)>>16;  /*地址的高8位,共24位*/
            AddrMid = (FontAddr&0xff00)>>8;      /*地址的中8位,共24位*/
            AddrLow = FontAddr&0xff;         /*地址的低8位,共24位*/
            get_n_bytes_data_from_ROM(AddrHigh,AddrMid,AddrLow,FontBuf,n );/*取一个汉字的数据，存到"FontBuf[]"*/
            Display_Asc(x,y,zk_num,trans, fc,bc);/*显示一个ascii到LCD上 */

            x+=d; //下一个字坐标
            i+=1;  //下个字符
        }
        else
        {
            //无法显示的字符

            x+=d; //下一个字坐标
            i+=2;  //下个字符
        }
	}

}

/******************************************************************************
      函数说明：显示ASCII码
      入口数据：x,y      写入的坐标
                zk_num   1:5*7   2:7*8   3:6*12,  4:8*16,  5:12*24,  6:16*32
                fc 字体颜色
                bc 背景颜色
      返回值：  无
******************************************************************************/
void Display_Asc(u16 x,u16 y,u8 zk_num, u8 trans, u16 fc,u16 bc)
{

    u8 i=0,j=0,k=0,n=0,x0=0,y0=0,m=0;
    u16 clr=0;

	switch(zk_num)
	{
        // n:字符所占字节数  d：字间距
        case 7:   n=7;  x0=5;  y0=7;  break;	 //	  5x7 ASCII
        case 8:   n=8;  x0=7;  y0=8;  break;	 //   7x8 ASCII
        case 12:  n=12; x0=6;  y0=12; break;	 //  6x12 ASCII
        case 16:  n=16; x0=8;  y0=16; break;	 //  8x16 ASCII
        case 24:  n=48; x0=12; y0=24; break;	 //  12x24 ASCII
        case 32:  n=64; x0=16; y0=32; break;	 //  16x32 ASCII
        default:return;
	}

	for(i=0;i<n;i++)
	{
		for(k=0;k<8;k++)
		{
			if((FontBuf[i]&(0x80>>k)))
            {
                clr=fc;
            }
            else
            {
                if(trans>0) continue;
                clr=bc;
            }

            if(x0<=8)
            {
                SetBitColor(x+k, y+i, clr);
            }
            else if(x0>8 && x0<=16)
            {
                if((i%2)!=0)
                {
                    SetBitColor(x+k+8, y+i/2, clr);
                }
                else
                {
                    SetBitColor(x+k, y+i/2, clr);
                }
            }
		}
	}
}

/******************************************************************************
      函数说明：显示ASCII码
      入口数据：x,y      写入的坐标
                zk_num   7:5*7   8:7*8   12:6*12,  16:8*16,  24:12*24,  32:16*32
                text[]   要显示的字符串
                fc 字体颜色
                bc 背景颜色
      返回值：  无
******************************************************************************/
void Display_Asc_String(u16 x,u16 y,u16 zk_num, u8 trans,u8 text[],u16 fc,u16 bc)
{
    u8 i= 0;
    u8 AddrHigh=0,AddrMid=0,AddrLow=0 ; //字高、中、低地址
    u32 FontAddr=0; //字地址
    u32 BaseAdd=0; //字库基地址
    u8 n=0,d=0;// 不同点阵字库的计算变量

    zk_init();//先初始化

    switch(zk_num)
    {
        //n个数，d:字间距
        case 7:   BaseAdd=0x1DDF80; n=8;  d=6;  break;	 //	  5x7 ASCII
        case 8:   BaseAdd=0x1DE280; n=8;  d=7;  break;	 //   7x8 ASCII
        case 12:  BaseAdd=0x1DBE00; n=12; d=6;  break;	 //  6x12 ASCII
        case 16:  BaseAdd=0x1DD780; n=16; d=8;  break;	 //  8x16 ASCII
        case 24:  BaseAdd=0x1DFF00; n=48; d=12; break;	 //  12x24 ASCII
        case 32:  BaseAdd=0x1E5A50; n=64; d=16; break;	 //  16x32 ASCII
    }

    while((text[i]>0x00))
    {
        if((text[i] >= 0x20) &&(text[i] <= 0x7E))
        {
            FontAddr = 	text[i]-0x20;
            FontAddr = (unsigned long)((FontAddr*n)+BaseAdd);

            AddrHigh = (FontAddr&0xff0000)>>16;  /*地址的高8位,共24位*/
            AddrMid = (FontAddr&0xff00)>>8;      /*地址的中8位,共24位*/
            AddrLow = FontAddr&0xff;	     /*地址的低8位,共24位*/
            get_n_bytes_data_from_ROM(AddrHigh,AddrMid,AddrLow,FontBuf,n );/*取一个汉字的数据，存到"FontBuf[]"*/
            Display_Asc(x,y,zk_num, trans, fc,bc);/*显示一个ascii到LCD上 */
        }
        i++;  //下个数据
        x+=d;//下一个字坐标
    }
}


/******************************************************************************
      函数说明：显示ASCII码(Arial&Times New Roman)
      入口数据：x,y      写入的坐标
                zk_num   1:8*12,  2:12*16,  3:16*24,  4:24*32
                fc 字体颜色
                bc 背景颜色
      返回值：  无
******************************************************************************/
void Display_Arial_TimesNewRoman(u16 x,u16 y,u8 zk_num,u16 fc,u16 bc)
{
#if 0
  u8 i,k,n,x0,y0,m=0;
	switch(zk_num)
	{
		// n:字符所占字节数  d:字间距
		case 12:  n=26;  x0=10; y0=12; break;	 //  8x12 ASCII
	  case 16:  n=34;  x0=12; y0=16; break;	 //  12x16 ASCII
	  case 24:  n=74;  x0=20; y0=24; break;	 //  16x24 ASCII
	 	case 32:  n=130; x0=25; y0=32; break;	 //  24x32 ASCII
	}
	LCD_Address_Set(x,y,x+x0-1,y+y0-1);
	for(i=2;i<n;i++)
	{
		for(k=0;k<8;k++)
		{
			if((FontBuf[i]&(0x80>>k)))
			{
			  LCD_WR_DATA(fc);
			}
			else
			{
			  LCD_WR_DATA(bc);
			}
			m++;
			if(m%x0==0)
			{
				m=0;
				break;
			}
		}
	}
#endif
}



/******************************************************************************
      函数说明：显示ASCII(Arial类型)
      入口数据：x,y      写入的坐标
                zk_num   1:8*12,  2:12*16,  3:16*24,  4:24*32
                text[]   要显示的字符串
                fc 字体颜色
                bc 背景颜色
      返回值：  无
******************************************************************************/
void Display_Arial_String(u16 x,u16 y,u16 zk_num,u8 text[],u16 fc,u16 bc)
{
	u8 i= 0;
	u8 AddrHigh=0,AddrMid=0,AddrLow=0 ; //字高、中、低地址
	u32 FontAddr=0; //字地址
	u32 BaseAdd=0; //字库基地址
  u8 n=0,d=0;// 不同点阵字库的计算变量
	switch(zk_num)
	{
		//n:个数，d:字间距
		case 12:  BaseAdd=0x1DC400; n=26;  d=10; break;	 //  8x12 ASCII(Arial类型)
	  case 16:  BaseAdd=0x1DE580; n=34;  d=12; break;	 //  12x16 ASCII(Arial类型)
	  case 24:  BaseAdd=0x1E22D0; n=74;  d=20; break;	 //  16x24 ASCII(Arial类型)
	 	case 32:  BaseAdd=0x1E99D0; n=130; d=25; break;	 //  24x32 ASCII(Arial类型)
	}
	while((text[i]>0x00))
	{
	  if((text[i] >= 0x20) &&(text[i] <= 0x7E))
		{
		  FontAddr = 	text[i]-0x20;
			FontAddr = (unsigned long)((FontAddr*n)+BaseAdd);

			AddrHigh = (FontAddr&0xff0000)>>16;  /*地址的高8位,共24位*/
			AddrMid = (FontAddr&0xff00)>>8;      /*地址的中8位,共24位*/
			AddrLow = FontAddr&0xff;	     /*地址的低8位,共24位*/
			get_n_bytes_data_from_ROM(AddrHigh,AddrMid,AddrLow,FontBuf,n );/*取一个汉字的数据，存到"FontBuf[]"*/
			Display_Arial_TimesNewRoman(x,y,zk_num,fc,bc);/*显示一个ascii到LCD上 */
		}
    i++;  //下个数据
		x+=d;//下一个字坐标
	}
}


/******************************************************************************
      函数说明：显示ASCII(Arial类型)
      入口数据：x,y      写入的坐标
                zk_num   1:8*12,  2:12*16,  3:16*24,  4:24*32
                text[]   要显示的字符串
                fc 字体颜色
                bc 背景颜色
      返回值：  无
******************************************************************************/
void Display_TimesNewRoman_String(u16 x,u16 y,u16 zk_num,u8 text[],u16 fc,u16 bc)
{
	u8 i= 0;
	u8 AddrHigh=0,AddrMid=0,AddrLow=0 ; //字高、中、低地址
	u32 FontAddr=0; //字地址
	u32 BaseAdd=0; //字库基地址
  u8 n=0,d=0;// 不同点阵字库的计算变量
	switch(zk_num)
	{
		//n:个数，d:字间距
		case 12:  BaseAdd=0x1DCDC0; n=26;  d=10; break;	 //  8x12 ASCII(TimesNewRoman类型)
	  case 16:  BaseAdd=0x1DF240; n=34;  d=12; break;	 //  12x16 ASCII(TimesNewRoman类型)
	  case 24:  BaseAdd=0x1E3E90; n=74;  d=20; break;	 //  16x24 ASCII(TimesNewRoman类型)
	 	case 32:  BaseAdd=0x1ECA90; n=130; d=25; break;	 //  24x32 ASCII(TimesNewRoman类型)
	}
	while((text[i]>0x00))
	{
	  if((text[i] >= 0x20) &&(text[i] <= 0x7E))
		{
		  FontAddr = 	text[i]-0x20;
			FontAddr = (unsigned long)((FontAddr*n)+BaseAdd);
			AddrHigh = (FontAddr&0xff0000)>>16;  /*地址的高8位,共24位*/
			AddrMid = (FontAddr&0xff00)>>8;      /*地址的中8位,共24位*/
			AddrLow = FontAddr&0xff;	     /*地址的低8位,共24位*/
			get_n_bytes_data_from_ROM(AddrHigh,AddrMid,AddrLow,FontBuf,n );/*取一个汉字的数据，存到"FontBuf[]"*/
			Display_Arial_TimesNewRoman(x,y,zk_num,fc,bc);/*显示一个ascii到LCD上 */
		}
    i++;  //下个数据
		x+=d;//下一个字坐标
	}
}

void Gui_DrawFont_GBK16(u16 x,u16 y,u16 fc,u16 bc, u8 trans, u8* text)
{
    u8 buff[50]={0};
    u8 len=strlen((char*)text);
    u8 maxBytes=lcd_getLineMaxByte(16);

    if(text==NULL) return;
    if(len>maxBytes)
    {
        memcpy(buff, text, maxBytes);
    }
    else
    {
        memset(buff, ' ', maxBytes);
        memcpy(buff, text, len);
    }

    Display_GB2312_String(x,y,16, trans, (u8*)buff, fc, bc);
}

void Gui_DrawFont_GBK24(u16 x,u16 y,u16 fc,u16 bc, u8 trans, u8* text)
{
    u8 buff[50]={0};
    u8 len=strlen((char*)text);
    u8 maxBytes=lcd_getLineMaxByte(24);

    if(text==NULL) return;
    if(len>maxBytes)
    {
        memcpy(buff, text, maxBytes);
    }
    else
    {
        memset(buff, ' ', maxBytes);
        memcpy(buff, text, len);
    }

    Display_GB2312_String(x,y,24, trans, (u8*)buff, fc, bc);
}

void Gui_DrawFont_GBK32(u16 x,u16 y,u16 fc,u16 bc, u8 trans, u8* text)
{
    u8 buff[50]={0};
    u8 len=strlen((char*)text);
    u8 maxBytes=lcd_getLineMaxByte(32);

    if(text==NULL) return;
    if(len>maxBytes)
    {
        memcpy(buff, text, maxBytes);
    }
    else
    {
        memset(buff, ' ', maxBytes);
        memcpy(buff, text, len);
    }

    Display_GB2312_String(x,y,32, trans, (u8*)buff, fc, bc);
}


