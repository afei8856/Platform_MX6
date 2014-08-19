/*
 * RockChip ADC Battery Driver 
 * Copyright (C) 2012, RockChip
 *
 * Authors: xuhuicong <xhc@rock-chips.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

 //#define DEBUG    1

#include <linux/platform_device.h>
#include <linux/power_supply.h>
#include <linux/workqueue.h>
#include <linux/kernel.h>
#include <linux/i2c.h>
#include <mach/gpio.h>
#include <linux/delay.h>
#include <linux/power/cw2015_battery.h>
#include <linux/time.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
//#include <mach/board.h>
#include <linux/input.h>
#include <linux/slab.h>
#include  <linux/imx6sl_evk/imx6sl_evk_gpio_cfg.h>


#define GPIOPullUp  1
#define GPIO_HIGH   1
#define GPIO_LOW    0

#define REG_VERSION             0x0
#define REG_VCELL               0x2
#define REG_SOC                 0x4
#define REG_RRT_ALERT           0x6
#define REG_CONFIG              0x8
#define REG_MODE                0xA
#define REG_BATINFO             0x10

#define MODE_SLEEP_MASK         (0x3<<6)
#define MODE_SLEEP              (0x3<<6)
#define MODE_NORMAL             (0x0<<6)
#define MODE_QUICK_START        (0x3<<4)
#define MODE_RESTART            (0xf<<0)

#define CONFIG_UPDATE_FLG       (0x1<<1)
#define ATHD                    (0xf<<3)        /*ATHD = 15%*/

#define CW_I2C_SPEED            100000          /* default i2c speed set 100khz*/
#define BATTERY_UP_MAX_CHANGE   420             /* the max time allow battery change quantity*/
#define BATTERY_DOWN_CHANGE   60                /*the max time allow battery change quantity*/
#define BATTERY_DOWN_MIN_CHANGE_RUN 30          /* the min time allow battery change quantity when run*/
#define BATTERY_DOWN_MIN_CHANGE_SLEEP 1800      /* the min time allow battery change quantity when run 30min*/

#define BATTERY_DOWN_MAX_CHANGE_RUN_AC_ONLINE 3600

#define NO_STANDARD_AC_BIG_CHARGE_MODE 1
//#define SYSTEM_SHUTDOWN_VOLTAGE  3400000        //set system shutdown voltage related in battery info.
#define BAT_LOW_INTERRUPT    1

#define USB_CHARGER_MODE        1
#define AC_CHARGER_MODE         2

//extern int dwc_otg_check_dpdm(void);
//extern int get_gadget_connect_flag(void);
//extern int dwc_vbus_status( void );
//#define SABRESD_CW2015_DC_ONLINE	IMX_GPIO_NR(6, 14)

//extern int i2c_master_reg8_recv(const struct i2c_client *client, const char reg, char *buf, int count, int scl_rate);
//extern int i2c_master_reg8_send(const struct i2c_client *client, const char reg, const char *buf, int count, int scl_rate);

static struct input_dev *input_dev;
//static volatile int cw2015_bal_online;
extern bool bUsbConnect;


struct cw_battery {
        struct i2c_client *client;
        struct workqueue_struct *battery_workqueue;
        struct delayed_work battery_delay_work;
        struct delayed_work dc_wakeup_work;
        struct delayed_work bat_low_wakeup_work;
        const struct cw_bat_platform_data *plat_data;

	struct input_dev *input;

        struct power_supply rk_bat;
        struct power_supply rk_ac;
        struct power_supply rk_usb;

        long sleep_time_capacity_change;      /* the sleep time from capacity change to present, it will set 0 when capacity change */
        long run_time_capacity_change;

        long sleep_time_charge_start;      /* the sleep time from insert ac to present, it will set 0 when insert ac */
        long run_time_charge_start;

        int dc_online;
        int usb_online;
        int charger_mode;
        int charger_init_mode;
        int capacity;
        int voltage;
        int status;
        int time_to_empty;
        int alt;

        int bat_change;
};
//static int i2c_master_reg8_recv(const struct i2c_client *client, const char reg, char *buf, int count, int scl_rate)
static int i2c_master_reg8_recv(const struct i2c_client *client, const char reg, char *buf, int count)
{
	struct i2c_adapter *adap=client->adapter;
	struct i2c_msg msgs[2];
	int ret;
	char reg_buf = reg;
	
	msgs[0].addr = client->addr;
	msgs[0].flags = client->flags;
	msgs[0].len = 1;
	msgs[0].buf = &reg_buf;
	//msgs[0].scl_rate = scl_rate;
	//msgs[0].udelay = client->udelay;

	msgs[1].addr = client->addr;
	msgs[1].flags = client->flags | I2C_M_RD;
	msgs[1].len = count;
	msgs[1].buf = (char *)buf;
	//msgs[1].scl_rate = scl_rate;
	//msgs[1].udelay = client->udelay;

	ret = i2c_transfer(adap, msgs, 2);

	return (ret == 2)? count : ret;
}

EXPORT_SYMBOL(i2c_master_reg8_recv);
//int i2c_master_reg8_send(const struct i2c_client *client, const char reg, const char *buf, int count, int scl_rate)
static int i2c_master_reg8_send(const struct i2c_client *client, const char reg, const char *buf, int count)	
{
	struct i2c_adapter *adap=client->adapter;
	struct i2c_msg msg;
	int ret;
	char *tx_buf = (char *)kmalloc(count + 1, GFP_KERNEL);
	if(!tx_buf)
		return -ENOMEM;
	tx_buf[0] = reg;
	memcpy(tx_buf+1, buf, count); 

	msg.addr = client->addr;
	msg.flags = client->flags;
	msg.len = count + 1;
	msg.buf = (char *)tx_buf;
	//msg.scl_rate = scl_rate;
	//msg.udelay = client->udelay;

	ret = i2c_transfer(adap, &msg, 1);
	kfree(tx_buf);
	
	return (ret == 1) ? count : ret;

}
EXPORT_SYMBOL(i2c_master_reg8_send);

static s32 cw2015_write_reg(struct i2c_client *client,u8 reg, u8 *val)
{
	u8 au8Buf[2] = {0};

	au8Buf[0] = reg ;

	au8Buf[1] = *val;

	if (i2c_master_send(client, au8Buf,2) < 0) {
		pr_err("%s:write reg error:reg=%x,val=%x\n",
			__func__, reg, au8Buf[1]);
		return -1;
	}

	return 0;
}

static s8 cw2015_read_reg(struct i2c_client *client,u8 reg, u8 *val)
{
	u8 au8RegBuf = 0;
	u8 u8RdVal = 0;

	au8RegBuf = reg ;
	

	if (1 != i2c_master_send(client, &au8RegBuf, 1)) {
		pr_err("%s:write reg error:reg=%x\n",
				__func__, reg);
		return -1;
	}

	if (1 != i2c_master_recv(client, &u8RdVal, 1)) {
		pr_err("%s:read reg error:reg=%x,val=%x\n",
				__func__, reg, u8RdVal);
		return -1;
	}

	*val = u8RdVal;

	return u8RdVal;
}


static int cw_read(struct i2c_client *client, u8 reg, u8 buf[])
{
        int ret;
	//ret = i2c_master_reg8_recv(client, reg, buf, 1, CW_I2C_SPEED);
	ret = i2c_master_reg8_recv(client, reg, buf, 1);
        return ret;
}

static int cw_write(struct i2c_client *client, u8 reg, u8 const buf[])
{
        int ret;
	//ret = i2c_master_reg8_send(client, reg, buf, 1, CW_I2C_SPEED);
	ret = i2c_master_reg8_send(client, reg, buf, 1);
        return ret;
}

static int cw_read_word(struct i2c_client *client, u8 reg, u8 buf[])
{
        int ret;
        // ret = i2c_master_reg8_recv(client, reg, buf, 2, CW_I2C_SPEED);
	   ret = i2c_master_reg8_recv(client, reg, buf, 2);
        return ret;
}


//#if 0
//static int cw_write_word(struct i2c_client *client, u8 reg, u8 const buf[])
//{
//        int ret;
//        ret = i2c_master_reg8_send(client, reg, buf, 2, CW_I2C_SPEED);
//        return ret;
//}
//#endif



static int cw_update_config_info(struct cw_battery *cw_bat)
{
        int ret;
        u8 reg_val;
        int i;
        u8 reset_val;

        dev_info(&cw_bat->client->dev, "func: %s-------\n", __func__);
        
        /* make sure no in sleep mode */
        ret = cw_read(cw_bat->client, REG_MODE, &reg_val);
        if (ret < 0)
                return ret;

        reset_val = reg_val;
        if((reg_val & MODE_SLEEP_MASK) == MODE_SLEEP) {
                dev_err(&cw_bat->client->dev, "Error, device in sleep mode, cannot update battery info\n");
                return -1;
        }

        /* update new battery info */
        for (i = 0; i < SIZE_BATINFO; i++) {
                dev_dbg(&cw_bat->client->dev, "cw_bat->plat_data->cw_bat_config_info[%d] = 0x%x\n", i, \
                                cw_bat->plat_data->cw_bat_config_info[i]);
                ret = cw_write(cw_bat->client, REG_BATINFO + i, &cw_bat->plat_data->cw_bat_config_info[i]);

                if (ret < 0) 
                  return ret;
        }
        /* readback & check */
        for (i = 0; i < SIZE_BATINFO; i++) {
                ret = cw_read(cw_bat->client, REG_BATINFO + i, &reg_val);
                if (reg_val != cw_bat->plat_data->cw_bat_config_info[i])
                        return -1;
        }
        
        /* set cw2015/cw2013 to use new battery info */
        ret = cw_read(cw_bat->client, REG_CONFIG, &reg_val);
        if (ret < 0)
                return ret;

        reg_val |= CONFIG_UPDATE_FLG;   /* set UPDATE_FLAG */
        reg_val &= 0x07;                /* clear ATHD */
        reg_val |= ATHD;                /* set ATHD */
        ret = cw_write(cw_bat->client, REG_CONFIG, &reg_val);
        if (ret < 0)
                return ret;

        /* check 2015/cw2013 for ATHD & update_flag */ 
        ret = cw_read(cw_bat->client, REG_CONFIG, &reg_val);
        if (ret < 0)
                return ret;
        
        if (!(reg_val & CONFIG_UPDATE_FLG)) {
                dev_info(&cw_bat->client->dev, "update flag for new battery info have not set..\n");
        }

        if ((reg_val & 0xf8) != ATHD) {
                dev_info(&cw_bat->client->dev, "the new ATHD have not set..\n");
        }

        /* reset */
        reset_val &= ~(MODE_RESTART);
        reg_val = reset_val | MODE_RESTART;
        ret = cw_write(cw_bat->client, REG_MODE, &reg_val);
        if (ret < 0)
                return ret;

        msleep(10);
        ret = cw_write(cw_bat->client, REG_MODE, &reset_val);
        if (ret < 0)
                return ret;
        
        return 0;
}

static int cw_init(struct cw_battery *cw_bat)
{
        int ret;
        int i;
        u8 reg_val = MODE_SLEEP;

	//printk("%s %s %d   \n",__FILE__,__func__,__LINE__);
	//#if 0
	//        ret = cw_read(cw_bat->client, REG_MODE, &reg_val);
	//        if (ret < 0)
	//               return ret;
	//#endif
        if ((reg_val & MODE_SLEEP_MASK) == MODE_SLEEP) {
                reg_val = MODE_NORMAL;
                ret = cw_write(cw_bat->client, REG_MODE, &reg_val);
                if (ret < 0) 
                {
                        return ret;
                }
        }

        ret = cw_read(cw_bat->client, REG_CONFIG, &reg_val);
        if (ret < 0)
                return ret;

        if ((reg_val & 0xf8) != ATHD) {
                dev_info(&cw_bat->client->dev, "the new ATHD have not set\n");
                reg_val &= 0x07;    /* clear ATHD */
                reg_val |= ATHD;    /* set ATHD */
                ret = cw_write(cw_bat->client, REG_CONFIG, &reg_val);
                if (ret < 0)
                        return ret;
        }

        ret = cw_read(cw_bat->client, REG_CONFIG, &reg_val);
        if (ret < 0) 
                return ret;

        if (!(reg_val & CONFIG_UPDATE_FLG)) {
                dev_info(&cw_bat->client->dev, "update flag for new battery info have not set\n");
                ret = cw_update_config_info(cw_bat);
                if (ret < 0)
                        return ret;
        } else {
                for(i = 0; i < SIZE_BATINFO; i++) { 
                        ret = cw_read(cw_bat->client, (REG_BATINFO + i), &reg_val);
                        if (ret < 0)
                              return ret;
                        if (cw_bat->plat_data->cw_bat_config_info[i] != reg_val)
                                break;
                }

                if (i != SIZE_BATINFO) {
                        dev_info(&cw_bat->client->dev, "update flag for new battery info have not set\n"); 
                        ret = cw_update_config_info(cw_bat);
                        if (ret < 0)
                                return ret;
                }
        }

        for (i = 0; i < 30; i++) {
                ret = cw_read(cw_bat->client, REG_SOC, &reg_val);
		//printk("%s %s  %d,i=%d ret=%d reg_val=0x%d   \n",__FILE__,__func__,__LINE__,i,ret,reg_val);
                if (ret < 0)
                        return ret;
                else if (reg_val <= 0x64) 
                        break;
                
                msleep(100);
                if (i > 25)
                        dev_err(&cw_bat->client->dev, "cw2015/cw2013 input unvalid power error\n");

        }

        if (i >=30){
             reg_val = MODE_SLEEP;
             ret = cw_write(cw_bat->client, REG_MODE, &reg_val);
             return -1;
        }
        return 0;
}

static void cw_update_time_member_charge_start(struct cw_battery *cw_bat)
{
        struct timespec ts;
        int new_run_time;
        int new_sleep_time;

        ktime_get_ts(&ts);
        new_run_time = ts.tv_sec;

        get_monotonic_boottime(&ts);
        new_sleep_time = ts.tv_sec - new_run_time;

        cw_bat->run_time_charge_start = new_run_time;
        cw_bat->sleep_time_charge_start = new_sleep_time; 
}

static void cw_update_time_member_capacity_change(struct cw_battery *cw_bat)
{
        struct timespec ts;
        int new_run_time;
        int new_sleep_time;

        ktime_get_ts(&ts);
        new_run_time = ts.tv_sec;

        get_monotonic_boottime(&ts);
        new_sleep_time = ts.tv_sec - new_run_time;

        cw_bat->run_time_capacity_change = new_run_time;
        cw_bat->sleep_time_capacity_change = new_sleep_time; 
}

static int cw_quickstart(struct cw_battery *cw_bat)
{
        int ret = 0;
        u8 reg_val = MODE_QUICK_START;

        ret = cw_write(cw_bat->client, REG_MODE, &reg_val);
        if(ret < 0) {
                dev_err(&cw_bat->client->dev, "Error quick start1\n");
                return ret;
        }
        
        reg_val = MODE_NORMAL;
        ret = cw_write(cw_bat->client, REG_MODE, &reg_val);
        if(ret < 0) {
                dev_err(&cw_bat->client->dev, "Error quick start2\n");
                return ret;
        }

        return 1;
}
#if 0
#define BAT_FAST_DOWN_CAP_THRESHOLD 5
#define BAT_FAST_DOWN_VOL_THRESHOLD 3640000
#define VOL_DELTA 10000
static bool fast_down_flag = false;
#endif
static int cw_get_capacity(struct cw_battery *cw_bat)
{
        int cw_capacity;
        int ret;
        u8 reg_val[2];

        struct timespec ts;
        long new_run_time;
        long new_sleep_time;
        long capacity_or_aconline_time;
        int allow_change;
        int allow_capacity;
        static int if_quickstart = 0;
        static int jump_flag =0;
        static int reset_flag =1; 
        static int reset_loop =0;
        int charge_time;
        u8 reset_val1;
        u8 reset_val2;
        int loop =0;

         //ret = cw_read(cw_bat->client, REG_SOC, &reg_val);
        ret = cw_read_word(cw_bat->client, REG_SOC, reg_val);
        if (ret < 0)
                return ret;
		
        cw_capacity = reg_val[0];
        if ((cw_capacity < 0) || (cw_capacity > 100)) {
                dev_err(&cw_bat->client->dev, "get cw_capacity error; cw_capacity = %d\n", cw_capacity);
                reset_loop++;
                
            if ((reset_flag ==1)&&(reset_loop >5)){ 
            	
            	  reset_flag =0;
            	  reset_loop =0;
                reset_val1 = MODE_SLEEP;
                reset_val2 = MODE_NORMAL;
                ret = cw_write(cw_bat->client, REG_MODE, &reset_val1);
                if (ret < 0)
                    return ret;

                msleep(10);
                ret = cw_write(cw_bat->client, REG_MODE, &reset_val2);
                if (ret < 0)
                    return ret;
                                              
                ret = cw_update_config_info(cw_bat);
                if (ret < 0)
                     return ret;               
            }
                                     
                return cw_capacity;
        }else {
        	reset_loop =0;
        }

        if (cw_capacity == 0) 
                dev_dbg(&cw_bat->client->dev, "the cw201x capacity is 0 !!!!!!!, funciton: %s, line: %d\n", __func__, __LINE__);
        else 
                dev_dbg(&cw_bat->client->dev, "the cw201x capacity is %d, funciton: %s\n", cw_capacity, __func__);

        // ret = cw_read(cw_bat->client, REG_SOC + 1, &reg_val);
        //printk("[%s] cap_reg = %i; cap_save = %i; cm = %i; v = %i\n",__func__,cw_capacity,cw_bat->capacity,cw_bat->charger_mode,cw_bat->voltage);

        ktime_get_ts(&ts);
        new_run_time = ts.tv_sec;

        get_monotonic_boottime(&ts);
        new_sleep_time = ts.tv_sec - new_run_time;
        
        if (((cw_bat->charger_mode > 0) && 
		(cw_capacity <= (cw_bat->capacity - 1)) 
		&& (cw_capacity > (cw_bat->capacity - 9)))
                 || ((cw_bat->charger_mode == 0)
                 && (cw_capacity == (cw_bat->capacity + 1)))
                 ) {/*modify battery level swing*/

	                if (!(cw_capacity == 0 && cw_bat->capacity == 1)) {			
		                    cw_capacity = cw_bat->capacity;
		            }
		}        

        if ((cw_bat->charger_mode > 0) 
		&& (cw_capacity >= 95)
		&& (cw_capacity <= cw_bat->capacity)) {/*avoid no charge full*/

                capacity_or_aconline_time = (cw_bat->sleep_time_capacity_change > cw_bat->sleep_time_charge_start) ? cw_bat->sleep_time_capacity_change : cw_bat->sleep_time_charge_start;
                capacity_or_aconline_time += (cw_bat->run_time_capacity_change > cw_bat->run_time_charge_start) ? cw_bat->run_time_capacity_change : cw_bat->run_time_charge_start;
                allow_change = (new_sleep_time + new_run_time - capacity_or_aconline_time) / BATTERY_UP_MAX_CHANGE;
                if (allow_change > 0) {
                        allow_capacity = cw_bat->capacity + allow_change; 
                        cw_capacity = (allow_capacity <= 100) ? allow_capacity : 100;
                        jump_flag =1;
                } else if (cw_capacity <= cw_bat->capacity) {
                        cw_capacity = cw_bat->capacity; 
                }

        } else if ((cw_bat->charger_mode == 0) 
	        && (cw_capacity <= cw_bat->capacity ) 
	        && (cw_capacity >= 90) 
	        && (jump_flag == 1)) {/*avoid battery level jump to CW_BAT*/
                capacity_or_aconline_time = (cw_bat->sleep_time_capacity_change > cw_bat->sleep_time_charge_start) ? cw_bat->sleep_time_capacity_change : cw_bat->sleep_time_charge_start;
                capacity_or_aconline_time += (cw_bat->run_time_capacity_change > cw_bat->run_time_charge_start) ? cw_bat->run_time_capacity_change : cw_bat->run_time_charge_start;
                allow_change = (new_sleep_time + new_run_time - capacity_or_aconline_time) / BATTERY_DOWN_CHANGE;
                if (allow_change > 0) {
                        allow_capacity = cw_bat->capacity - allow_change; 
                        if (cw_capacity >= allow_capacity){
                        	jump_flag =0;
                        }
                        else{
                                cw_capacity = (allow_capacity <= 100) ? allow_capacity : 100;
                        }
                } else if (cw_capacity <= cw_bat->capacity) {
                        cw_capacity = cw_bat->capacity;
                }
        }

		if ((cw_capacity == 0) && (cw_bat->capacity > 1)) { /* avoid battery level jump to 0% at a moment from more than 2%*/
                allow_change = ((new_run_time - cw_bat->run_time_capacity_change) / BATTERY_DOWN_MIN_CHANGE_RUN);
                allow_change += ((new_sleep_time - cw_bat->sleep_time_capacity_change) / BATTERY_DOWN_MIN_CHANGE_SLEEP);

                allow_capacity = cw_bat->capacity - allow_change;
                cw_capacity = (allow_capacity >= cw_capacity) ? allow_capacity: cw_capacity;
              //  dev_info(&cw_bat->client->dev, "report GGIC POR happened");//add by zyh20130927
                reg_val[0] = MODE_NORMAL;
                ret = cw_write(cw_bat->client, REG_MODE, reg_val);
                if (ret < 0)
                        return ret;
        }
#if 0
		static bool first_enter = true;

        if ((cw_bat->charger_mode == 0 && (cw_bat->voltage <= BAT_FAST_DOWN_VOL_THRESHOLD)) || fast_down_flag) { /* avoid battery level jump to 0% at a moment from more than 2%*/
        	int v2c = 0;
        	fast_down_flag = true;

        	if (cw_bat->voltage > BAT_FAST_DOWN_VOL_THRESHOLD - VOL_DELTA) {
        		v2c = 7;
        	} else if (cw_bat->voltage > (BAT_FAST_DOWN_VOL_THRESHOLD - 2 * VOL_DELTA) ) {
        		v2c = 5;
        	} else if (cw_bat->voltage > (BAT_FAST_DOWN_VOL_THRESHOLD - 5 * VOL_DELTA) ) {
        		v2c = 4;
        	} else
        		v2c = 3;

        	if (first_enter)
        		cw_bat->capacity = (v2c + 1);

        	cw_capacity = cw_bat->capacity > v2c ? cw_bat->capacity - 1 : cw_bat->capacity;

        }
        first_enter = false;
        if (cw_bat->charger_mode > 0)
        	fast_down_flag = false;
#endif

#if 1	
	if((cw_bat->charger_mode > 0) &&(cw_capacity == 0))
	{		  
                charge_time = new_sleep_time + new_run_time - cw_bat->sleep_time_charge_start - cw_bat->run_time_charge_start;
                if ((charge_time > BATTERY_DOWN_MAX_CHANGE_RUN_AC_ONLINE) && (if_quickstart == 0)) {
        		      cw_quickstart(cw_bat);      // if the cw_capacity = 0 the cw2015 will qstrt
        		      dev_info(&cw_bat->client->dev, "report battery capacity still 0 if in changing");
                        if_quickstart = 1;
                }
	} else if ((if_quickstart == 1)&&(cw_bat->charger_mode == 0)) {
    		if_quickstart = 0;
        }

#endif

#if 0
        if (cw_bat->plat_data->chg_ok_pin != INVALID_GPIO) {
                if(gpio_get_value(cw_bat->plat_data->chg_ok_pin) != cw_bat->plat_data->chg_ok_level) {
                        if (cw_capacity == 100) {
                                cw_capacity = 99;
                        }
                } else {
                        if (cw_bat->charger_mode > 0) {
                                cw_capacity = 100;
                        }
                }
        }
#endif

#ifdef SYSTEM_SHUTDOWN_VOLTAGE
        if ((cw_bat->charger_mode == 0) 
		&& (cw_capacity <= 20) 
	       && (cw_bat->voltage <= SYSTEM_SHUTDOWN_VOLTAGE)){      	     
                if (if_quickstart == 10){  
                	
                    allow_change = ((new_run_time - cw_bat->run_time_capacity_change) / BATTERY_DOWN_MIN_CHANGE_RUN);
                    allow_change += ((new_sleep_time - cw_bat->sleep_time_capacity_change) / BATTERY_DOWN_MIN_CHANGE_SLEEP);

                    allow_capacity = cw_bat->capacity - allow_change;
                    cw_capacity = (allow_capacity >= 0) ? allow_capacity: 0;
                	
                    if (cw_capacity < 1){	     	      	
                        cw_quickstart(cw_bat);
                        if_quickstart = 12;
                        cw_capacity = 0;
                    }
                } else if (if_quickstart <= 10)
                        if_quickstart =if_quickstart+2;
                dev_info(&cw_bat->client->dev, "the cw201x voltage is less than SYSTEM_SHUTDOWN_VOLTAGE !!!!!!!, funciton: %s, line: %d\n", __func__, __LINE__);
        } else if ((cw_bat->charger_mode > 0)&& (if_quickstart <= 12)) {
                if_quickstart = 0;
        }
#endif
        return cw_capacity;
}

static int cw_get_vol(struct cw_battery *cw_bat)
{
        int ret;
        u8 reg_val[2];
        int value16, value16_1, value16_2, value16_3;
        long voltage;

        ret = cw_read_word(cw_bat->client, REG_VCELL, reg_val);
        if (ret < 0)
                return ret;
        value16 = (reg_val[0] << 8) + reg_val[1];
        
        ret = cw_read_word(cw_bat->client, REG_VCELL, reg_val);
        if (ret < 0)
                return ret;
        value16_1 = (reg_val[0] << 8) + reg_val[1];

        ret = cw_read_word(cw_bat->client, REG_VCELL, reg_val);
        if (ret < 0)
                return ret;
        value16_2 = (reg_val[0] << 8) + reg_val[1];
		
		
        if(value16 > value16_1)
	{	 
	    	value16_3 = value16;
		    value16 = value16_1;
		    value16_1 = value16_3;
        }
		
        if(value16_1 > value16_2)
	{
	    	value16_3 =value16_1;
			value16_1 =value16_2;
			value16_2 =value16_3;
	 }
			
        if(value16 >value16_1)
	{	 
	    	value16_3 =value16;
			value16 =value16_1;
			value16_1 =value16_3;
        }			

        voltage = ((long)value16_1) * 312 / 1024;
        voltage = voltage * 1000;/*uv*/

	//printk("cw_get_vol   voltage =%d\n",voltage);
	
        return voltage;
}

#ifdef BAT_LOW_INTERRUPT
static int cw_get_alt(struct cw_battery *cw_bat)
{
        int ret = 0;
        u8 reg_val;
        u8 value8 = 0;
        int alrt;
        
        ret = cw_read(cw_bat->client, REG_RRT_ALERT, &reg_val);
        if (ret < 0)
                return ret;
        value8 = reg_val;
        alrt = value8 >>7;
        
        //dev_info(&cw_bat->client->dev, "read RRT %d%%. value16 0x%x\n", alrt, value16);
        value8 = value8&0x7f;
        reg_val = value8;
        ret = cw_write(cw_bat->client, REG_RRT_ALERT, &reg_val);
        if(ret < 0) {
                dev_err(&cw_bat->client->dev, "Error clear ALRT\n");
                return ret;
        }
        
        return alrt;
}
#endif


static int cw_get_time_to_empty(struct cw_battery *cw_bat)
{
        int ret;
        u8 reg_val;
        u16 value16;

        ret = cw_read(cw_bat->client, REG_RRT_ALERT, &reg_val);
        if (ret < 0)
                return ret;

        value16 = reg_val;

        ret = cw_read(cw_bat->client, REG_RRT_ALERT + 1, &reg_val);
        if (ret < 0)
                return ret;

        value16 = ((value16 << 8) + reg_val) & 0x1fff;
        return value16;
}

static void rk_bat_update_capacity(struct cw_battery *cw_bat)
{
        int cw_capacity;

        cw_capacity = cw_get_capacity(cw_bat);
	//printk("%s %s %d,cw_bat->capacity=%d, cw_capacity=%d  \n",__FILE__,__func__,__LINE__,cw_bat->capacity,cw_capacity);
        if ((cw_capacity >= 0) && (cw_capacity <= 100) && (cw_bat->capacity != cw_capacity)) {
                cw_bat->capacity = cw_capacity;
                cw_bat->bat_change = 1;
                cw_update_time_member_capacity_change(cw_bat);

                if (cw_bat->capacity == 0)
                        dev_info(&cw_bat->client->dev, "report battery capacity 0 and will shutdown if no changing");

        }
}



static void rk_bat_update_vol(struct cw_battery *cw_bat)
{
        int ret;
        ret = cw_get_vol(cw_bat);
        if ((ret >= 0) && (cw_bat->voltage != ret)) {
                cw_bat->voltage = ret;
                cw_bat->bat_change = 1;
        }
}

static void rk_bat_update_status(struct cw_battery *cw_bat)
{
        int status;

        if (cw_bat->charger_mode > 0) {
                if (cw_bat->capacity >= 100) 
                        status=POWER_SUPPLY_STATUS_FULL;
                else
                        status=POWER_SUPPLY_STATUS_CHARGING;
        } else {
                status = POWER_SUPPLY_STATUS_NOT_CHARGING;
        }

        if (cw_bat->status != status) {
                cw_bat->status = status;
                cw_bat->bat_change = 1;
        } 
}

static void rk_bat_update_time_to_empty(struct cw_battery *cw_bat)
{
        int ret;
        ret = cw_get_time_to_empty(cw_bat);
        if ((ret >= 0) && (cw_bat->time_to_empty != ret)) {
                cw_bat->time_to_empty = ret;
                cw_bat->bat_change = 1;
        }
        
}
static int rk_ac_update_online(struct cw_battery *cw_bat)
{
        int ret = 0;
	
    /*  if(cw_bat->plat_data->dc_det_pin == MX6SL_INVALID_GPIO) {
                cw_bat->dc_online = 0;
                return 0;
        } */
//#if 0
 //       if (cw_bat->plat_data->is_dc_charge == 0) {
 //               cw_bat->dc_online = 0;
 //               return 0;
 //       }
//#endif
	//printk("%s %s %d,bUsbConnect=%d  \n",__FILE__,__func__,__LINE__,bUsbConnect);

       if (bUsbConnect){
	//if(gpio_get_value(cw_bat->plat_data->dc_det_pin) == cw_bat->plat_data->dc_det_level) {
                if (cw_bat->dc_online != 1) {
                        cw_update_time_member_charge_start(cw_bat);
                        cw_bat->dc_online = 1;
                        if (cw_bat->charger_mode != AC_CHARGER_MODE)
                                cw_bat->charger_mode = AC_CHARGER_MODE;
 
                        ret = 1;
                }
        } else {
                if (cw_bat->dc_online != 0) {
                        cw_update_time_member_charge_start(cw_bat);
                        cw_bat->dc_online = 0;
                        if (cw_bat->usb_online == 0)
                                cw_bat->charger_mode = 0;
                        ret = 1;
                }
        }
        return ret;
}

static int get_usb_charge_state(struct cw_battery *cw_bat)
{
        int charge_time;
        int time_from_boot;
        struct timespec ts;

        int gadget_status = 0;
        int usb_status = 0;

        get_monotonic_boottime(&ts);
        time_from_boot = ts.tv_sec;
        
        if (cw_bat->charger_init_mode) {
 
                if (usb_status == 1 || usb_status == 2) {
                        cw_bat->charger_init_mode = 0;
                } else if (time_from_boot < 8) {
                        usb_status = cw_bat->charger_init_mode;
                } else if (strstr(saved_command_line,"charger")) {
                        cw_bat->charger_init_mode = 0;
                        usb_status = cw_bat->charger_init_mode;
                }
        }
#if 0 
        if (cw_bat->usb_online == 1) {
                
                charge_time = time_from_boot - cw_bat->sleep_time_charge_start - cw_bat->run_time_charge_start;
                if (charge_time > 3) {
                        if (gadget_status == 0 && dwc_vbus_status() == 1) {
                                usb_status = 2;
                        }
                }
        }
#endif
        return usb_status;
 
        dev_dbg(&cw_bat->client->dev, "%s usb_status=[%d],cw_bat->charger_mode=[%d],cw_bat->gadget_status=[%d], cw_bat->charger_init_mode = [%d]\n",__func__,usb_status,cw_bat->charger_mode,gadget_status, cw_bat->charger_init_mode);



}

static int rk_usb_update_online(struct cw_battery *cw_bat)
{
        int ret = 0;
        int usb_status = 0;
      
        if (cw_bat->plat_data->is_usb_charge == 0) {
                cw_bat->usb_online = 0;
                return 0;

        }
        
        usb_status = get_usb_charge_state(cw_bat);        
        if (usb_status == 2) {
                if (cw_bat->charger_mode != AC_CHARGER_MODE) {
                        cw_bat->charger_mode = AC_CHARGER_MODE;
                        ret = 1;
                }
                if (cw_bat->plat_data->chg_mode_sel_pin != NULL) {
                        if (gpio_get_value (cw_bat->plat_data->chg_mode_sel_pin) != cw_bat->plat_data->chg_mode_sel_level)
                                gpio_direction_output(cw_bat->plat_data->chg_mode_sel_pin, (cw_bat->plat_data->chg_mode_sel_level==1) ? 1 : 0);
                }
                
                if (cw_bat->usb_online != 1) {
                        cw_bat->usb_online = 1;
                        cw_update_time_member_charge_start(cw_bat);
                }
                
        } else if (usb_status == 1) {
                if ((cw_bat->charger_mode != USB_CHARGER_MODE) && (cw_bat->dc_online == 0)) {
                        cw_bat->charger_mode = USB_CHARGER_MODE;
                        ret = 1;
                }
                
                if (cw_bat->plat_data->chg_mode_sel_pin != NULL) {
                        if (gpio_get_value (cw_bat->plat_data->chg_mode_sel_pin) == cw_bat->plat_data->chg_mode_sel_level)
                                gpio_direction_output(cw_bat->plat_data->chg_mode_sel_pin, (cw_bat->plat_data->chg_mode_sel_level==1) ? 0 : 1);
                }
                if (cw_bat->usb_online != 1){
                        cw_bat->usb_online = 1;
                        cw_update_time_member_charge_start(cw_bat);
                }

        } else if (usb_status == 0 && cw_bat->usb_online != 0) {

                if (cw_bat->plat_data->chg_mode_sel_pin != NULL) {
                        if (gpio_get_value (cw_bat->plat_data->chg_mode_sel_pin == cw_bat->plat_data->chg_mode_sel_level))
                                gpio_direction_output(cw_bat->plat_data->chg_mode_sel_pin, (cw_bat->plat_data->chg_mode_sel_level==1) ? 0 : 1);
                }

                if (cw_bat->dc_online == 0)
                        cw_bat->charger_mode = 0;

                cw_update_time_member_charge_start(cw_bat);
                cw_bat->usb_online = 0;
                ret = 1;
        }

        return ret;
}

static void cw_bat_work(struct work_struct *work)
{
        struct delayed_work *delay_work;
        struct cw_battery *cw_bat;
        int ret,cw2015_vol,power_online;
	u8 reg_val[2];

        delay_work = container_of(work, struct delayed_work, work);
        cw_bat = container_of(delay_work, struct cw_battery, battery_delay_work);

	//add by zyh20131010
	//gpio_request(SABRESD_CW2015_DC_ONLINE, "power-online");
	//gpio_direction_input(SABRESD_CW2015_DC_ONLINE);
	//power_online = gpio_get_value(SABRESD_CW2015_DC_ONLINE);
	//printk("SABRESD_CW2015_DC_ONLINE  SABRESD_CW2015_DC_ONLINE power_online = %d\n",power_online);
#if 0///wen del,   
	cw2015_vol = cw_get_vol(cw_bat);
	//printk("%s %s %d cw2015_vol=%d  \n",__FILE__,__func__,__LINE__,cw2015_vol);
	//printk("cw2015_vol  cw2015_vol cw2015_vol = %d\n",cw2015_vol);
        if ((cw2015_vol < 2000000) /*&& !power_online*/)
        {
        	cw2015_bal_online = 0;
		 //rk_bat_update_vol(cw_bat);
	//	goto DF_DC_ONLINE;
        }
	if ((cw2015_vol > 4000000) && (!cw2015_bal_online))
	{
		printk(" run to cw_quickstart  cw_quickstart   cw_quickstart  cw_quickstart\n");
		cw2015_bal_online =1;
		ret=cw_quickstart(cw_bat);
	}
#endif
	//gpio_free(SABRESD_CW2015_DC_ONLINE);
	//printk("111111111111111111cw_bat_work  cw_bat_work  cw_bat_work\n");
        ret = rk_ac_update_online(cw_bat);
        if (ret == 1) {
                power_supply_changed(&cw_bat->rk_ac);
        }
	//printk("%s %s %d cw_bat->plat_data->is_usb_charge=%d  \n",__FILE__,__func__,__LINE__,cw_bat->plat_data->is_usb_charge);
        if (cw_bat->plat_data->is_usb_charge == 1) {
                ret = rk_usb_update_online(cw_bat);
                if (ret == 1) {
                        power_supply_changed(&cw_bat->rk_usb);     
                        power_supply_changed(&cw_bat->rk_ac);
                }
        }


        rk_bat_update_status(cw_bat);
        rk_bat_update_vol(cw_bat);
        rk_bat_update_capacity(cw_bat);
        rk_bat_update_time_to_empty(cw_bat);
	//printk("%s %s %d cw_bat->bat_change=%d  \n",__FILE__,__func__,__LINE__,cw_bat->bat_change);
        if (cw_bat->bat_change) {
                power_supply_changed(&cw_bat->rk_bat);
                cw_bat->bat_change = 0;
        }

      
	goto WORK_OK;
//DF_DC_ONLINE:
       // if (SABRESD_CW2015_DC_ONLINE != NULL)
       //         gpio_free(SABRESD_CW2015_DC_ONLINE);

WORK_OK:
	queue_delayed_work(cw_bat->battery_workqueue, &cw_bat->battery_delay_work, 
	   msecs_to_jiffies(5*1000));
	//	  dev_err(&cw_bat->client->dev, "cw_bat->charge_mode = %d, cw_bat->time_to_empty = %d, cw_bat->capacity = %d, cw_bat->voltage = %d, cw_bat->dc_online = %d, cw_bat->usb_online = %d\n",\
	//					  cw_bat->charger_mode, cw_bat->time_to_empty, cw_bat->capacity, cw_bat->voltage, cw_bat->dc_online, cw_bat->usb_online);

        
}

static int rk_usb_get_property (struct power_supply *psy,
                enum power_supply_property psp,
                union power_supply_propval *val)
{
        int ret = 0;
        struct cw_battery *cw_bat;

        cw_bat = container_of(psy, struct cw_battery, rk_usb);
        switch (psp) {
        case POWER_SUPPLY_PROP_ONLINE:
                // val->intval = cw_bat->usb_online;
                val->intval = (cw_bat->charger_mode == USB_CHARGER_MODE);   
                break;
        default:
                break;
        }
        return ret;
}

static enum power_supply_property rk_usb_properties[] = {
        POWER_SUPPLY_PROP_ONLINE,
};


static int rk_ac_get_property (struct power_supply *psy,
                enum power_supply_property psp,
                union power_supply_propval *val)
{
        int ret = 0;
        struct cw_battery *cw_bat;

        cw_bat = container_of(psy, struct cw_battery, rk_ac);
        switch (psp) {
        case POWER_SUPPLY_PROP_ONLINE:
                // val->intval = cw_bat->dc_online;
                val->intval = (cw_bat->charger_mode == AC_CHARGER_MODE);
                break;
        default:
                break;
        }
        return ret;
}

static enum power_supply_property rk_ac_properties[] = {
        POWER_SUPPLY_PROP_ONLINE,
};

static int rk_battery_get_property(struct power_supply *psy,
                enum power_supply_property psp,
                union power_supply_propval *val)
{
        int ret = 0;
        struct cw_battery *cw_bat;

        cw_bat = container_of(psy, struct cw_battery, rk_bat); 
        switch (psp) {
        case POWER_SUPPLY_PROP_CAPACITY:
                val->intval = cw_bat->capacity;
                break;
        case POWER_SUPPLY_PROP_STATUS:
                val->intval = cw_bat->status;
                break;
                
        case POWER_SUPPLY_PROP_HEALTH:
                val->intval= POWER_SUPPLY_HEALTH_GOOD;
                break;
        case POWER_SUPPLY_PROP_PRESENT:
                val->intval = cw_bat->voltage <= 0 ? 0 : 1;
                break;
                
        case POWER_SUPPLY_PROP_VOLTAGE_NOW:
                val->intval = cw_bat->voltage;
                break;
                
        case POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW:
                val->intval = cw_bat->time_to_empty;			
                break;
            
        case POWER_SUPPLY_PROP_TECHNOLOGY:
                val->intval = POWER_SUPPLY_TECHNOLOGY_LION;	
                break;

        default:
                break;
        }
        return ret;
}

static enum power_supply_property rk_battery_properties[] = {
        POWER_SUPPLY_PROP_CAPACITY,
        POWER_SUPPLY_PROP_STATUS,
        POWER_SUPPLY_PROP_HEALTH,
        POWER_SUPPLY_PROP_PRESENT,
        POWER_SUPPLY_PROP_VOLTAGE_NOW,
        POWER_SUPPLY_PROP_TIME_TO_EMPTY_NOW,
        POWER_SUPPLY_PROP_TECHNOLOGY,
};

static int cw_bat_gpio_init(struct cw_battery *cw_bat)
{

        int ret;
	printk("%s %s %d cw_bat->plat_data->dc_det_pin=%d, cw_bat->plat_data->bat_low_pin=%d, cw_bat->plat_data->chg_ok_pin=%d, cw_bat->plat_data->chg_mode_sel_pin=%d\n",
		__FILE__,__func__,__LINE__,
		cw_bat->plat_data->dc_det_pin,
		cw_bat->plat_data->bat_low_pin,
		cw_bat->plat_data->chg_ok_pin,
		cw_bat->plat_data->chg_mode_sel_pin);

        //gpio_free(cw_bat->plat_data->dc_det_pin);
        if (cw_bat->plat_data->dc_det_pin != MX6SL_INVALID_GPIO) {
		gpio_free(cw_bat->plat_data->dc_det_pin);
                ret = gpio_request(cw_bat->plat_data->dc_det_pin, NULL);
                if (ret) {
                        dev_err(&cw_bat->client->dev, "failed to request dc_det_pin gpio\n");
                        goto request_dc_det_pin_fail;
                }

                gpio_set_value(cw_bat->plat_data->dc_det_pin, GPIOPullUp);
                ret = gpio_direction_input(cw_bat->plat_data->dc_det_pin);
                if (ret) {
                        dev_err(&cw_bat->client->dev, "failed to set dc_det_pin input\n");
                        goto request_bat_low_pin_fail;
                }
        }
        if (cw_bat->plat_data->bat_low_pin != MX6SL_INVALID_GPIO) {
                ret = gpio_request(cw_bat->plat_data->bat_low_pin, NULL);
                if (ret) {
                        dev_err(&cw_bat->client->dev, "failed to request bat_low_pin gpio\n");
                        goto request_bat_low_pin_fail;
                }

                gpio_set_value(cw_bat->plat_data->bat_low_pin, GPIOPullUp);
                ret = gpio_direction_input(cw_bat->plat_data->bat_low_pin);
                if (ret) {
                        dev_err(&cw_bat->client->dev, "failed to set bat_low_pin input\n");
                        goto request_chg_ok_pin_fail;
                }
        }
        if (cw_bat->plat_data->chg_ok_pin != MX6SL_INVALID_GPIO) {
                ret = gpio_request(cw_bat->plat_data->chg_ok_pin, NULL);
                if (ret) {
                        dev_err(&cw_bat->client->dev, "failed to request chg_ok_pin gpio\n");
                        goto request_chg_ok_pin_fail;
                }

                gpio_set_value(cw_bat->plat_data->chg_ok_pin, GPIOPullUp);
                ret = gpio_direction_input(cw_bat->plat_data->chg_ok_pin);
                if (ret) {
                        dev_err(&cw_bat->client->dev, "failed to set chg_ok_pin input\n");
                        gpio_free(cw_bat->plat_data->chg_ok_pin); 
                        goto request_chg_ok_pin_fail;
                }
        }

        if ((cw_bat->plat_data->is_usb_charge == 1) && 
			(cw_bat->plat_data->chg_mode_sel_pin!= MX6SL_INVALID_GPIO)) {
                ret = gpio_request(cw_bat->plat_data->chg_mode_sel_pin, NULL);
                if (ret) {
                        dev_err(&cw_bat->client->dev, "failed to request chg_mode_sel_pin gpio\n");
                        goto request_chg_ok_pin_fail;
                }
                ret = gpio_direction_output(cw_bat->plat_data->chg_mode_sel_pin, (cw_bat->plat_data->chg_mode_sel_level==1) ? 0 : 1);
                if (ret) {
                        dev_err(&cw_bat->client->dev, "failed to set chg_mode_sel_pin input\n");
                        gpio_free(cw_bat->plat_data->chg_mode_sel_pin); 
                        goto request_chg_ok_pin_fail;
                }
        }
 
        return 0;

        
request_chg_ok_pin_fail:
        if (cw_bat->plat_data->bat_low_pin != NULL)
                gpio_free(cw_bat->plat_data->bat_low_pin);

request_bat_low_pin_fail:
        if (cw_bat->plat_data->dc_det_pin != NULL) 
                gpio_free(cw_bat->plat_data->dc_det_pin);

request_dc_det_pin_fail:
        return ret;

}


static void dc_detect_do_wakeup(struct work_struct *work)
{
#if 1

        int ret;
        int irq;
        unsigned int type;

        struct delayed_work *delay_work;
        struct cw_battery *cw_bat;

        delay_work = container_of(work, struct delayed_work, work);
        cw_bat = container_of(delay_work, struct cw_battery, dc_wakeup_work);

	//add by zyh20130912
        //rk28_send_wakeup_key();
        
	if (!input_dev)
		return;
	//if(cw2015_wake_up_state)
//	{
		//input_report_key(input_dev, KEY_WAKEUP, 1);
		//input_sync(input_dev);
		//cw2015_wake_up_state = 0;
	//	printk("dc_detect_do_wakeup11111111111111111111111111111111\n");
	//}
	///else
	//{
		input_report_key(input_dev, KEY_WAKEUP, 0);
		input_sync(input_dev);
	//	cw2015_wake_up_state = 1;
	//	printk("dc_detect_do_wakeup000000000000000000000000000000000\n");
	//}
		//end
		
        /* this assume if usb insert or extract dc_det pin is change */
	//#if 0
	//        if(cw_bat->charger_init_mode)
	//                cw_bat->charger_init_mode=0;
	//#endif

        irq = gpio_to_irq(cw_bat->plat_data->dc_det_pin);
        type = gpio_get_value(cw_bat->plat_data->dc_det_pin) ? IRQ_TYPE_EDGE_FALLING : IRQ_TYPE_EDGE_RISING;
        ret = irq_set_irq_type(irq, type);
        if (ret < 0) {
                pr_err("%s: irq_set_irq_type(%d, %d) failed\n", __func__, irq, type);
        }
        enable_irq(irq);
#endif
}


static irqreturn_t dc_detect_irq_handler(int irq, void *dev_id)
{
        struct cw_battery *cw_bat = dev_id;
        disable_irq_nosync(irq); // for irq debounce
        queue_delayed_work(cw_bat->battery_workqueue, &cw_bat->dc_wakeup_work, msecs_to_jiffies(20));
        return IRQ_HANDLED;
}

#ifdef BAT_LOW_INTERRUPT

#define WAKE_LOCK_TIMEOUT       (10 * HZ)
#if defined(CONFIG_WAKELOCK)
static struct wake_lock bat_low_wakelock;
#endif

static void bat_low_detect_do_wakeup(struct work_struct *work)
{
        struct delayed_work *delay_work;
        struct cw_battery *cw_bat;

        delay_work = container_of(work, struct delayed_work, work);
        cw_bat = container_of(delay_work, struct cw_battery, bat_low_wakeup_work);
        dev_info(&cw_bat->client->dev, "func: %s-------\n", __func__);
        cw_get_alt(cw_bat);
        //enable_irq(irq);
}

static irqreturn_t bat_low_detect_irq_handler(int irq, void *dev_id)
{
        struct cw_battery *cw_bat = dev_id;
        // disable_irq_nosync(irq); // for irq debounce
#if  defined(CONFIG_WAKELOCK        )
        wake_lock_timeout(&bat_low_wakelock, WAKE_LOCK_TIMEOUT);
#endif
        queue_delayed_work(cw_bat->battery_workqueue, &cw_bat->bat_low_wakeup_work, msecs_to_jiffies(20));
        return IRQ_HANDLED;
}
#endif

static int cw_bat_probe(struct i2c_client *client, const struct i2c_device_id *id)
{
        struct cw_battery *cw_bat;
        int ret;
        int irq;
        int irq_flags;
        int loop = 0;
	struct input_dev *input;

	struct device *dev = &client->dev;

        cw_bat = devm_kzalloc(&client->dev, sizeof(*cw_bat), GFP_KERNEL);
	//cw_bat = kzalloc(sizeof(*cw_bat), GFP_KERNEL);
        if (!cw_bat) {
                dev_err(&cw_bat->client->dev, "fail to allocate memory\n");
                return -ENOMEM;
        }
        //////////input devices ////////////////////////////////
	input = input_allocate_device();
	if (!input) {
		 dev_err(&cw_bat->client->dev, "fail to input_allocate_device\n");
		return -ENOMEM;
	}

	input->name = client->name;
	input->phys = "gpio-keys/input0";
	input->dev.parent = dev;

	input->id.bustype = BUS_HOST;
	input->id.vendor = 0x0001;
	input->id.product = 0x0001;
	input->id.version = 0x0100;

	/// Enable auto repeat feature of Linux input subsystem /
	__set_bit(EV_REP, input->evbit);

	cw_bat->input = input;
        ///////////////////////////////////////////////////	
        cw_bat->client = client;

        cw_bat->plat_data = client->dev.platform_data;
        i2c_set_clientdata(client, cw_bat);

        ret = cw_bat_gpio_init(cw_bat);
        if (ret) {
                dev_err(&cw_bat->client->dev, "cw_bat_gpio_init error\n");
                return ret;
        }


        ret = cw_init(cw_bat);
        while ((loop++ < 200) && (ret != 0)) {
                ret = cw_init(cw_bat);
        }

        if (ret) 
             return ret;
        
        cw_bat->rk_bat.name = "battery";
        cw_bat->rk_bat.type = POWER_SUPPLY_TYPE_BATTERY;
        cw_bat->rk_bat.properties = rk_battery_properties;
        cw_bat->rk_bat.num_properties = ARRAY_SIZE(rk_battery_properties);
        cw_bat->rk_bat.get_property = rk_battery_get_property;
        ret = power_supply_register(&client->dev, &cw_bat->rk_bat);
        if(ret < 0) {
        	dev_err(&cw_bat->client->dev, "power supply register rk_bat error\n");
        	goto rk_bat_register_fail;
        }

        cw_bat->rk_ac.name = "ac";
        cw_bat->rk_ac.type = POWER_SUPPLY_TYPE_MAINS;
        cw_bat->rk_ac.properties = rk_ac_properties;
        cw_bat->rk_ac.num_properties = ARRAY_SIZE(rk_ac_properties);
        cw_bat->rk_ac.get_property = rk_ac_get_property;
        ret = power_supply_register(&client->dev, &cw_bat->rk_ac);
        if(ret < 0) {
        	dev_err(&cw_bat->client->dev, "power supply register rk_ac error\n");
        	goto rk_ac_register_fail;
        }


        cw_bat->rk_usb.name = "usb";
        cw_bat->rk_usb.type = POWER_SUPPLY_TYPE_USB;
        cw_bat->rk_usb.properties = rk_usb_properties;
        cw_bat->rk_usb.num_properties = ARRAY_SIZE(rk_usb_properties);
        cw_bat->rk_usb.get_property = rk_usb_get_property;
        ret = power_supply_register(&client->dev, &cw_bat->rk_usb);
        if(ret < 0) {
        	dev_err(&cw_bat->client->dev, "power supply register rk_ac error\n");
        	goto rk_usb_register_fail;
        }

        cw_bat->charger_init_mode = 0;

        cw_bat->dc_online = 0;
        cw_bat->usb_online = 0;
        cw_bat->charger_mode = 0;
        cw_bat->capacity = 1;/*set default value is 1,make sure the devices can bootup */
        cw_bat->voltage = 0;
        cw_bat->status = 0;
        cw_bat->time_to_empty = 0;
        cw_bat->bat_change = 0;

        cw_update_time_member_capacity_change(cw_bat);
        cw_update_time_member_charge_start(cw_bat);

        cw_bat->battery_workqueue = create_singlethread_workqueue("rk_battery");
        INIT_DELAYED_WORK(&cw_bat->battery_delay_work, cw_bat_work);
        INIT_DELAYED_WORK(&cw_bat->dc_wakeup_work, dc_detect_do_wakeup);
        queue_delayed_work(cw_bat->battery_workqueue, &cw_bat->battery_delay_work, msecs_to_jiffies(10));
        if (cw_bat->plat_data->dc_det_pin != MX6SL_INVALID_GPIO) {
                irq = gpio_to_irq(cw_bat->plat_data->dc_det_pin);
                irq_flags = gpio_get_value(cw_bat->plat_data->dc_det_pin) ? IRQF_TRIGGER_FALLING : IRQF_TRIGGER_RISING;
                ret = request_irq(irq, dc_detect_irq_handler, irq_flags, "usb_detect", cw_bat);
                if (ret < 0) {
                        pr_err("%s: request_irq(%d) failed\n", __func__, irq);
                }
                enable_irq_wake(irq);
        }
	
#ifdef BAT_LOW_INTERRUPT
        INIT_DELAYED_WORK(&cw_bat->bat_low_wakeup_work, bat_low_detect_do_wakeup);
#if defined(CONFIG_WAKELOCK)
        wake_lock_init(&bat_low_wakelock, WAKE_LOCK_SUSPEND, "bat_low_detect");
#endif
        if (cw_bat->plat_data->bat_low_pin != MX6SL_INVALID_GPIO) {
                irq = gpio_to_irq(cw_bat->plat_data->bat_low_pin);
                ret = request_irq(irq, bat_low_detect_irq_handler, IRQF_TRIGGER_RISING, "bat_low_detect", cw_bat);
                if (ret < 0) {
                        gpio_free(cw_bat->plat_data->bat_low_pin);
                }
                enable_irq_wake(irq);
        }
#endif
         //////////////input/////////////////////////		
	input_set_capability(input, EV_KEY, KEY_WAKEUP);
	
	int error = input_register_device(input);
	if (error) {
		pr_err("gpio-keys: Unable to register input device, "
			"error: %d\n", error);
		return -1;
	}

	input_dev = input;
          //////////////////////////////////////////////////////////
        dev_info(&cw_bat->client->dev, "cw2015/cw2013 driver v1.2 probe sucess\n");
        return 0;

rk_usb_register_fail:
        power_supply_unregister(&cw_bat->rk_bat);
rk_ac_register_fail:
        power_supply_unregister(&cw_bat->rk_ac);
rk_bat_register_fail:
        dev_info(&cw_bat->client->dev, "cw2015/cw2013 driver v1.2 probe error!!!!\n");
        return ret;
}

static int cw_bat_remove(struct i2c_client *client)
{
        struct cw_battery *cw_bat = i2c_get_clientdata(client);
        dev_dbg(&cw_bat->client->dev, "%s\n", __func__);
        cancel_delayed_work(&cw_bat->battery_delay_work);
        return 0;
}

#ifdef CONFIG_PM
static int cw_bat_suspend(struct device *dev)
{
        struct i2c_client *client = to_i2c_client(dev);
        struct cw_battery *cw_bat = i2c_get_clientdata(client);
        dev_dbg(&cw_bat->client->dev, "%s\n", __func__);
        cancel_delayed_work(&cw_bat->battery_delay_work);
        return 0;
}

static int cw_bat_resume(struct device *dev)
{
        struct i2c_client *client = to_i2c_client(dev);
        struct cw_battery *cw_bat = i2c_get_clientdata(client);
        dev_dbg(&cw_bat->client->dev, "%s\n", __func__);
        queue_delayed_work(cw_bat->battery_workqueue, &cw_bat->battery_delay_work, msecs_to_jiffies(100));
        return 0;
}

MODULE_DEVICE_TABLE(i2c, cw_id);

static const struct dev_pm_ops cw_bat_pm_ops = {
        .suspend  = cw_bat_suspend,
        .resume   = cw_bat_resume,
};
#endif
static const struct i2c_device_id cw_id[] = {
	{ CW2015_BATTERY_DRIVE_NAME, 0 },
};

static struct i2c_driver cw_bat_driver = {
        .driver         = {
                .name   = CW2015_BATTERY_DRIVE_NAME,
#ifdef CONFIG_PM
                .pm     = &cw_bat_pm_ops,
#endif
        },
        
        .probe          = cw_bat_probe,
        .remove         = cw_bat_remove,
	.id_table	= cw_id,
};

static int __init cw_bat_init(void)
{
        return i2c_add_driver(&cw_bat_driver);
}

static void __exit cw_bat_exit(void)
{
        i2c_del_driver(&cw_bat_driver);
}

fs_initcall(cw_bat_init);
module_exit(cw_bat_exit);

MODULE_AUTHOR("ben.chen@cellwise-semi.com>");
MODULE_DESCRIPTION("cw2015/cw2013 battery driver");
MODULE_LICENSE("GPL");

