#include<string>
#include<memory>
#include<fstream>
#include<cmath>
#include<unistd.h>
#include<functional>
#include"ros/ros.h"
#include"roller_eye/data_publisher.h"
#include"sensor_msgs/Imu.h"
#include"sensor_msgs/Range.h"
#include"sensor_msgs/Illuminance.h"
#include"sensor_msgs/MagneticField.h"
#include"roller_eye/GyroSensor.h"
#include"roller_eye/MmaSensor.h"
#include"roller_eye/ProximitySensor.h"
#include"roller_eye/LightSensor.h"
#include"roller_eye/ros_tools.h"
#include"roller_eye/plog_plus.h"
#include"roller_eye/plt_assert.h"
#include"SparkFunLSM9DS1.h"
#include"vl53l0x_helper.h"
#include"rpi_ak09918.h"
#include"roller_eye/single_class.h"
#include"roller_eye/plt_config.h"
#include"roller_eye/system_define.h"
#include"roller_eye/util_class.h"
#include"roller_eye/status_publisher.h"
#include "roller_eye/imu_calib.h"
#include "zlog.h"

#define SENSOR_NODE_TAG  "SensorNode"

#define MAX_WAIT_TIMES          300
#define CALI_BUFFER_SIZE       150

#define DEG_TO_RAD    (M_PI/180)

#define TOF_OFFSET_CALI_MAX_TIMES   30
#define TOF_OFFSET_CALI_TIMES   20

static const std::string IMU_FRAME_ID="imu";

static const std::string TOF_OFFSET_FILE="tof_offset";

using namespace roller_eye;

class InitDZLog{
    public:
    InitDZLog()
    {
    	log_t arg = {
			confpath:	"/var/roller_eye/config/log/" SENSOR_NODE_TAG ".cfg",
			levelpath:	"/var/roller_eye/config/log/log.level",
			logpath:	"/var/log/" SENSOR_NODE_TAG ".log",
			cname:		SENSOR_NODE_TAG
		};
		if(0 != dzlogInit(&arg,2)){
			printf("%s log int error.\r\n",SENSOR_NODE_TAG);
		}

    }
    ~InitDZLog()
    {
		dzlogfInit();
    }
} ;


struct sensor_vector3f{
    uint64_t stamp;
    double x;
    double y;
    double z;
};

static void doVector3fOffsetCalibration(const char* type,const string& savePath,double calib[3],double base[3],double stationary[3],double shake[3],std::function<int(sensor_vector3f&)> getData)
{
     PLOG_DEBUG(SENSOR_NODE_TAG,"start %s calibration...\n",type);
    int idx=0,cnt;
    double preX(0.0),preY(0.0),preZ(0.0),sumX(0.0),sumY(0.0),sumZ(0.0);
    sensor_vector3f val;
    bool reset;
    const char* result="fail";

    for (cnt=0;cnt<MAX_WAIT_TIMES;cnt++){
        if(getData(val)!=0){
            continue;
        }
        reset=true;
        if(abs(val.x-base[0]) < stationary[0] &&  abs(val.y-base[1]) < stationary[1]  && abs(val.z-base[2]) < stationary[2] ){
            if(idx!=0){
                if(abs(val.x-preX)< shake[0] && abs(val.y-preY)<  shake[1]  && abs(val.z-preZ)<  shake[2] ){
                    reset=false;
                }
            }else{
                reset=false;
            }
        }

        if(reset){
            idx=0;
            sumX=sumY=sumZ=0.0;
            PLOG_INFO(SENSOR_NODE_TAG,"calibration reset\n");
        }else{
            preX=val.x;
            preY=val.y;
            preZ=val.z;
            sumX+=preX;
            sumY+=preY;
            sumZ+=preZ;
            idx++;
        }

        if(idx==CALI_BUFFER_SIZE){
            calib[0]=sumX/idx-base[0];
            calib[1]=sumY/idx-base[1];
            calib[2]=sumZ/idx-base[2];
            saveVector3fCalibration(savePath,calib);
            result="success";
            break;
        }           
    }

     PLOG_DEBUG(SENSOR_NODE_TAG,"%s calibrate %s !!!\n",type,result);    
}

class IMUData{
public:
    IMUData(double cali_gyro_static,double cali_gyro_shake,double cali_acc_static,double cali_acc_shake,double g):
    mHandle(""),
    GYRO_STATIC_THROLD(cali_gyro_static),
    GYRO_SHAKE_THROLD(cali_gyro_shake),
    ACC_STATIC_THROLD(cali_acc_static),
    ACC_SHAKE_THROLD(cali_acc_shake),
    G_VALUE(g)
    {
        clearCalibration();
        mCalibSrv = mHandle.advertiseService("/imu_calib",&IMUData::imu_calib,this);
    }
    virtual ~IMUData()
    {
    }
    virtual void start()=0;
    virtual void stop()=0;
    virtual int readGyro(sensor_vector3f &gyro)=0;
    virtual int readAcc(sensor_vector3f &acc)=0;
    int setData(sensor_msgs::Imu& s)
    {
        bool err=false,accOk=false,gyroOk=false;
        int diff;
        sensor_vector3f gyro,acc;
        while(true){
            if(!gyroOk){
                if(readGyro(gyro)!=0){
                    err=true;
                }else{
                   gyroOk=true; 
                }
            }
            if(!accOk){
                if(readAcc(acc)!=0){
                    err=true;
                }else{
                    accOk=true;
                }
            }
            if(err){
                return -1;
            }

            diff=abs((int64_t)gyro.stamp-(int64_t)acc.stamp)/1000000;
#ifdef SLAM_PRINTF_TIME_DIFF
             PLOG_DEBUG(SENSOR_NODE_TAG,"timestamp btween A&G is:%d",diff);
#endif
            if(diff>8){
                if(gyro.stamp>acc.stamp){//acc buff is larger than gyro buff
                    accOk=false;
                }else{
                    gyroOk=false;
                }
            }else{
                break;
            }
        }

        s.header.frame_id=IMU_FRAME_ID;
        s.header.stamp.fromNSec((gyro.stamp+acc.stamp)/2);

        s.orientation.w=1.0;

        s.angular_velocity.x=gyro.x-mGyroCali[0];
        s.angular_velocity.y=gyro.y-mGyroCali[1];
        s.angular_velocity.z=gyro.z-mGyroCali[2];

        s.linear_acceleration.x=acc.x-mAccCali[0];
        s.linear_acceleration.y=acc.y-mAccCali[1];
        s.linear_acceleration.z=acc.z-mAccCali[2];
        return 0;
    }
protected:
    bool doCalibration(bool useCalibra=true)
    {
        bool gyroCali,accCali;

        gyroCali=readVector3fCalibration(GYRO_CALIBRATION_FILE,mGyroCali);
        accCali=readVector3fCalibration(ACC_CALIBRATION_FILE,mAccCali);

        if(!gyroCali || !accCali){
             PLOG_INFO(SENSOR_NODE_TAG,"Disable motor....\n");
            auto dis=motor_disable(mHandle);

            start();
            if(!gyroCali){
                doGyroCalibraion();
            }
            if(!accCali){
                doAccCalibration();
            }
            stop();

            PLOG_INFO(SENSOR_NODE_TAG,"Enable motor....\n");
            motor_enable(dis);
        }
        if(!useCalibra){
            clearCalibration();
        }

        double gCali[3];
        double aCali[3];
        gyroCali=readVector3fCalibration(GYRO_CALIBRATION_FILE,gCali);
        accCali=readVector3fCalibration(ACC_CALIBRATION_FILE,aCali);
        if (gyroCali&&accCali){
            return true;
        }

        return false;
    }

    void doGyroCalibraion()
    {
        double base[]={0.0,0.0,0.0};
        double stationary[]={GYRO_STATIC_THROLD,GYRO_STATIC_THROLD,GYRO_STATIC_THROLD};
        double shake[]={GYRO_SHAKE_THROLD,GYRO_SHAKE_THROLD,GYRO_SHAKE_THROLD};

        doVector3fOffsetCalibration("GYRO",GYRO_CALIBRATION_FILE,mGyroCali,base,stationary,shake,[this](sensor_vector3f &val)->int{
            return this->readGyro(val);
        });
    }
    void doAccCalibration()
    {
        double base[]={0.0,0.0,G_VALUE};
        double stationary[]={ACC_STATIC_THROLD,ACC_STATIC_THROLD,ACC_STATIC_THROLD};
        double shake[]={ACC_SHAKE_THROLD,ACC_SHAKE_THROLD,ACC_SHAKE_THROLD};

        doVector3fOffsetCalibration("ACC",ACC_CALIBRATION_FILE,mAccCali,base,stationary,shake,[this](sensor_vector3f &val)->int{
            return this->readAcc(val);
        });
    }
    void clearCalibration()
    {
        mAccCali[0]=mAccCali[1]=mAccCali[2]=0.0;
        mGyroCali[0]=mGyroCali[1]=mGyroCali[2]=0.0;
    }

   bool imu_calib(imu_calibRequest& req,imu_calibResponse& res)
    {
        system(CMD_PREFIX"clean_imu_calib.sh");
        bool bStart = mStart;    
#ifndef SENSOR_IMU_USE_CALIBRA
        bool bRet = doCalibration(false);
#else
        bool bRet = doCalibration(true);
#endif
        if (bStart){
            start();
        }
        return bRet;
    }
    ros::NodeHandle mHandle;
    ros::ServiceServer mCalibSrv;
    double mGyroCali[3];
    double mAccCali[3];
    //calibrate param
    double GYRO_STATIC_THROLD; 
    double GYRO_SHAKE_THROLD; 
    double ACC_STATIC_THROLD;
    double ACC_SHAKE_THROLD;
    double G_VALUE;
protected:
    bool mStart = false;
};
class IMUDataFrom6Dof:public IMUData{
public:
    IMUDataFrom6Dof(bool flip):
    IMUData(5*0.10,5*0.05,5*0.45,5*0.22,9.7833),
    //IMUData(0.10,0.05,10,10,9.7833),
    mGyro(new GyroSensor()),
    mAcc(new MmaSensor()),
    mSkipCnt(50)
    {
        //virtual frame(fix),x->right,y->front,z->up
        //imu frame,flip=true,x->right,y->front,z->up,flip=false,,x->left,y->front,z->down
        mDir[0]=mDir[1]=mDir[2]=1;
        if(!flip){
            mDir[0]=-1;
            mDir[2]=-1;
        }
#ifndef SENSOR_IMU_USE_CALIBRA
        doCalibration(false);
#else
        doCalibration(true);
#endif
    }
    ~IMUDataFrom6Dof()
    {
        PLOG_INFO(SENSOR_NODE_TAG,"6 Dof IMU destroy");
    }
    void start()
    {        
        mStart = true;
        plt_assert(mGyro->enable(0,1)==0);
        plt_assert(mAcc->enable(0,1)==0);
        mAccSkip=mGyroSkip=0;
        PLOG_INFO(SENSOR_NODE_TAG,"start get 6dof imu data");
    }
    void stop()
    {
        mStart = false;
        mGyro->enable(0,0);
        mAcc->enable(0,0);
        PLOG_INFO(SENSOR_NODE_TAG,"stop get 6dof imu data");
    }
    
    int readGyro(sensor_vector3f &gyro)
    {
         sensors_event_t g;
        if(mGyro->readEvents(&g,1)!=1){
            return -1;
        }
        if(mGyroSkip<mSkipCnt){//drop the first data
            mGyroSkip++;
            return -1;
        }
#ifdef  SLAM_USE_KERNEL_STAMP
        gyro.stamp=g.timestamp;
#else
        gyro.stamp=ros::Time::now().toNSec();
#endif
        gyro.x=g.gyro.y*mDir[0];
        gyro.y=(-g.gyro.x)*mDir[1];
        gyro.z=g.gyro.z*mDir[2];

        return 0;
    }
    int readAcc(sensor_vector3f &acc)
    {
        sensors_event_t a;
         if(mAcc->readEvents(&a,1)!=1){
            return -1;
        }
        if(mAccSkip<mSkipCnt){//drop the first data
            mAccSkip++;
            PLOG_DEBUG(SENSOR_NODE_TAG,"Accel Skip:x=%f,y=%f,z=%f\n",a.acceleration.x,a.acceleration.y,a.acceleration.z);
            return -1;
        }
#ifdef  SLAM_USE_KERNEL_STAMP
        acc.stamp=a.timestamp;
#else
        acc.stamp=ros::Time::now().toNSec();
#endif
        acc.x=a.acceleration.x*mDir[0];
        acc.y=a.acceleration.y*mDir[1];
        acc.z=a.acceleration.z*mDir[2];
        return 0;
    }
private:
    unique_ptr<GyroSensor> mGyro;
    unique_ptr<MmaSensor> mAcc;
    int mSkipCnt;
    int mGyroSkip;
    int mAccSkip;
    int mDir[3];
};
// 
class Lsm9ds1IMU :public SingleClass<Lsm9ds1IMU>{
     friend class SingleClass<Lsm9ds1IMU>;
public:
    LSM9DS1 imu;
private:
Lsm9ds1IMU()
{
    plt_assert(imu.begin()!=0);
}
};
class IMUDataFrom9Dof:public IMUData{
public:
    IMUDataFrom9Dof():
    IMUData(0.10,0.05,0.45,0.22,9.7833),
    mIMU(Lsm9ds1IMU::getInstance()->imu)
    {
        doCalibration();
    }
    ~IMUDataFrom9Dof()
    {
        PLOG_INFO(SENSOR_NODE_TAG,"9 Dof IMU destroy");
    }
    void start()
    {
        PLOG_INFO(SENSOR_NODE_TAG,"start get 9dof imu data");
    }
    void stop()
    {
        PLOG_INFO(SENSOR_NODE_TAG,"stop get 9dof imu data");
    }
    
    int readGyro(sensor_vector3f &gyro)
    {
        mIMU.readGyro();
        gyro.stamp=ros::Time::now().toNSec();
        gyro.x=mIMU.calcGyro(mIMU.gx)*(-DEG_TO_RAD);
        gyro.y=mIMU.calcGyro(mIMU.gy)*DEG_TO_RAD;
        gyro.z=mIMU.calcGyro(mIMU.gz)*DEG_TO_RAD;
        return 0;
    }
    int readAcc(sensor_vector3f &acc)
    {
        mIMU.readAccel();
        acc.stamp=ros::Time::now().toNSec();
        acc.x=mIMU.calcAccel(mIMU.ax)*(-G_VALUE);
        acc.y=mIMU.calcAccel(mIMU.ay)*G_VALUE;
        acc.z=mIMU.calcAccel(mIMU.az)*G_VALUE;
        return 0;
    }
private:
    LSM9DS1 &mIMU;
};
class ToFData{
public:
    ToFData(uint8_t type,float min,float max,float arc,float calibraDis=0.2):
    mType(type),
    mMinRange(min),mMaxRange(max),
    mARC(arc),
    mCalibraDistance(calibraDis)
    {
        if(loadCalibration()!=0){
            PLOG_WARN(SENSOR_NODE_TAG,"load tof calibration fail\n");
            mOffset=0.0;
        }
    }
    virtual ~ToFData()
    {
    }
    virtual void start()=0;
    virtual void stop()=0;
    virtual int readDistance(float& distance)=0;

    int setData(sensor_msgs::Range& r)
    {
        float distance;
        if(readDistance(distance)!=0){
            return -1;
        }
      
        r.header.stamp=ros::Time::now();

        r.radiation_type=mType;
        r.field_of_view=mARC;
        r.min_range=mMinRange;
        r.max_range=mMaxRange;

        distance-=mOffset;
        if(distance<mMinRange){
            r.range=-INFINITY;
        }else if(distance>mMaxRange){
            r.range=INFINITY;
        }else{
            r.range=distance;
        }

        return 0;
    }
    virtual int loadCalibration(){
        return loadOffsetCalibration();
    }
    virtual int doCalibration(){
        return doOffsetCalibration();
    }
private:
    virtual int loadOffsetCalibration();
    virtual int doOffsetCalibration();
  
    const uint8_t mType;
    const float mMinRange,mMaxRange;
    const float mARC;
    float mOffset;
    float mCalibraDistance;
};
int ToFData::doOffsetCalibration()
{
    int count=0;
    float distance;
    float distanceSum=0.0;

    for(int i=0;i<TOF_OFFSET_CALI_MAX_TIMES;i++){
        if(readDistance(distance)!=0){
            continue;
        }
        distanceSum+=distance;
        if(++count==TOF_OFFSET_CALI_TIMES){
            distance=distanceSum/count;
        }
    }

    if(count==TOF_OFFSET_CALI_TIMES){
        ofstream file(string(SENSOR_CONFIG_PATH)+TOF_OFFSET_FILE);
        file<<distance-mCalibraDistance<<flush;
        return file.fail()?-1:0;
    }else{
        return -1;
    }
}
int ToFData::loadOffsetCalibration()
{
    ifstream file(string(SENSOR_CONFIG_PATH)+TOF_OFFSET_FILE);
    float offset;

    file>>offset;
    return file.fail()?-1:0;
}
class VL53L0xData:public ToFData{
public:
    VL53L0xData():ToFData(sensor_msgs::Range::INFRARED,VL53L0X_CAP_MIN,VL53L0X_CAP_MAX,VL53L0X_CAP_ARC)
    {
        plt_assert(tof_vl53l0x_init(&mTof)==0);
        plt_assert(tof_vl53l0x_setup_single(&mTof,HIGH_SPEED)==0);
    }
    ~VL53L0xData(){

    }
    virtual void start()
    {
        PLOG_INFO(SENSOR_NODE_TAG,"Start vl53l0x tof \n");
    }
    virtual void stop()
    {
        PLOG_INFO(SENSOR_NODE_TAG,"Stop vl53l0x tof \n");
    }
    virtual int readDistance(float& distance)
    {
        int val=tof_single_get_distance(&mTof);
        if(val<0){
            return -1;
        }
        distance=val/1000.0;
        return 0;
    }
private:
    VL53L0X_Dev_t mTof;
};
class VL53L0xDataFromKernel:public ToFData{
public:
    VL53L0xDataFromKernel():ToFData(sensor_msgs::Range::INFRARED,VL53L0X_CAP_MIN,VL53L0X_CAP_MAX,VL53L0X_CAP_ARC),
    mTof(new ProximitySensor())
    {
        
    }
    ~VL53L0xDataFromKernel()
    {

    }
    virtual void start()
    {
        PLOG_INFO(SENSOR_NODE_TAG,"Start vl53l0x tof \n");
        plt_assert(mTof->enable(0,1)==0);
    }
    virtual void stop()
    {
        PLOG_INFO(SENSOR_NODE_TAG,"Stop vl53l0x tof \n");
        plt_assert(mTof->enable(0,0)==0);
    }
    virtual int readDistance(float& distance)
    {
        sensors_event_t dis;
        if(mTof->readEvents(&dis,1)!=1){
            return -1;
        }
        distance=dis.distance/1000.0;
        return 0;
    }
private:
    unique_ptr<ProximitySensor> mTof;
};

class LightData{
public:
    LightData()
    {

    }
    virtual ~LightData()
    {

    }
    int setData(sensor_msgs::Illuminance& illum)
    {
        double data;
        if(readIlluminance(data)<0){
            return -1;
        }
        illum.header.stamp=ros::Time::now();
        illum.illuminance=data;
        illum.variance=0.0;
        return 0;
    }
    virtual void start()=0;
    virtual void stop()=0;
protected:
    virtual int readIlluminance(double& data)=0;
};
class LightDataFromKernel:public LightData{
public:
    LightDataFromKernel():
    mLight(new LightSensor())
    {

    }
    void start()
    {
        PLOG_INFO(SENSOR_NODE_TAG,"Start light sensor \n");
        plt_assert(mLight->enable(0,1)==0);
    }
    void stop()
    {
        PLOG_INFO(SENSOR_NODE_TAG,"Stop light sensor \n");
        plt_assert(mLight->enable(0,0)==0);
    }
private:
    int readIlluminance(double& data)
    {
        sensors_event_t light;
        if(mLight->readEvents(&light,1)!=1){
            return -1;
        }
        data=light.light;
        return 0;
    }
    unique_ptr<LightSensor> mLight;
};
//A simple battery status publisher
class BatteryStatus{
public:
    BatteryStatus(shared_ptr<ros::NodeHandle>&n):
    mStatus(*n,true,"simple_battery_status"),
    mPowerSupplyRoot("/sys/class/power_supply/rk-bat/"),
    mPreChargingStatus(status::BATTERY_UNCHARGE),
    mPreVol(-100)
    {
        mTimer=n->createTimer(ros::Duration(0.5),&BatteryStatus::timerCallback,this);
    }
private:
    void timerCallback(const ros::TimerEvent& evt)
    {
        std::ifstream chargeFile(mPowerSupplyRoot+"status");
        std::ifstream voltageFile(mPowerSupplyRoot+"voltage_now");
        int status[3];
        int voltage;
        string charge;
        chargeFile>>charge;
        voltageFile>>voltage;
        if(chargeFile.fail()||voltageFile.fail()){
            return;
        }
        if(charge=="Charging"){
           status[0]= status::BATTERY_CHARGING;
        }else if(charge=="Discharging"){
            status[0]= status::BATTERY_UNCHARGE;
        }else if(charge=="Full"){
            status[0]= status::BATTERY_FULL;
        }else{
            status[0]= status::BATTERY_UNKOWN;
        }
        status[2]=((status[0]==status::BATTERY_CHARGING||status[0]==status::BATTERY_FULL)?1:0);
        double volf=(float)voltage/2000000;

        if(volf<3.0){
            status[1]=0;
        }else if(volf<3.2){
            status[1]=(int)((volf-3.0)/0.2*15);
        }else if(volf<3.5){
            status[1]=15+(int)((volf-3.2)/0.3*15);
        }else if(volf<3.7){
            status[1]=30+(int)((volf-3.5)/0.2*45);
        }else if(volf<4.15){
            status[1]=75+(int)((volf-3.7)/0.45*25);
        }else{
            status[1]=100;
        }
        if(status[0]!=mPreChargingStatus||abs(status[1]-mPreVol)>=2){
            mPreChargingStatus=status[0];
            mPreVol=status[1];
            mStatus.pubStatus(status,sizeof(status)/sizeof(status[0]));
        }
    }
    ros::Timer mTimer;
    StatusPublisher mStatus;
    string mPowerSupplyRoot;
    int mPreChargingStatus;
    int mPreVol;
};

int main(int argc, char **argv)
{
    InitDZLog dzLog;
    ros::init(argc, argv, "SensorNode");
    ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME,SENSOR_NODE_DEBUG_LEVEL);
    auto n=make_shared<ros::NodeHandle>("~");
    DeviceDefaultConfig cfg;
  
    DataPulisher<sensor_msgs::Imu,IMUData> imu(n,"imu",2000,make_shared<IMUDataFrom6Dof>(cfg.getBoradFlipped()),0.0);
    DataPulisher<sensor_msgs::Range,ToFData> tof(n,"tof",20,make_shared<VL53L0xDataFromKernel>(),0.0);
    DataPulisher<sensor_msgs::Illuminance,LightData> light(n,"light",10,make_shared<LightDataFromKernel>(),0.0);

    BatteryStatus batStatus(n);
    ros::spin();
}

