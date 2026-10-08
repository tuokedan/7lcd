#ifndef _ZK_H_
#define _ZK_H_


#ifdef __cplusplus
extern "C" {
#endif


typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned long  u32;



//画笔颜色(BGR)
#define WHITE         	 0xFFFF
#define BLACK         	 0x0000
#define BLUE           	 0x1F00
#define BRED             0X1FF8
#define GRED 			 0XE0FF
#define GBLUE			 0XFF07
#define RED           	 0x00F8
#define MAGENTA       	 0x1FF8
#define GREEN         	 0xE007
#define CYAN          	 0xFF7F
#define YELLOW        	 0xE0FF
#define BROWN 			 0X40BC //棕色
#define BRRED 			 0X07FC //棕红色
#define GRAY  			 0X3084 //灰色
#define DARKBLUE      	 0XCF01	//深蓝色
#define LIGHTBLUE      	 0X7C7D	//浅蓝色
#define GRAYBLUE       	 0X5854 //灰蓝色
#define LIGHTGREEN     	 0X1F84 //浅绿色
#define LGRAY 			 0X18C6 //浅灰色(PANNEL),窗体背景色
#define LGRAYBLUE        0X51A6 //浅灰蓝色(中间层颜色)
#define LBBLUE           0X122B //浅棕蓝色(选择条目的反色)
#define VIOLET           0x1ff8  //紫色



void zk_init();
void Display_Asc_String(u16 x,u16 y,u16 zk_num, u8 trans,u8 text[],u16 fc,u16 bc);
void Display_GB2312_String(u16 x,u16 y,u8 zk_num, u8 trans,u8 text[],u16 fc,u16 bc);
void Gui_DrawFont_GBK16(u16 x,u16 y,u16 fc,u16 bc, u8 trans, u8 text[]);
void Gui_DrawFont_GBK24(u16 x,u16 y,u16 fc,u16 bc, u8 trans, u8 text[]);
void Gui_DrawFont_GBK32(u16 x,u16 y,u16 fc,u16 bc, u8 trans, u8* text);




#ifdef __cplusplus
}
#endif


#endif
