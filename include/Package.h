#pragma once

// C4251: 导出类的成员使用了无DLL导出接口的类型(std::string, cv::Mat等)，对本项目无实际影响
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#pragma warning(disable: 4251)
constexpr double PI = 3.14159265358979323846;
constexpr double VEL_C = 299792458.0;
constexpr int INPUTMAXSIZE = 1024;

/*-------------------------------------------------------*/
/*                  雷达收发模式枚举                     */
/*-------------------------------------------------------*/
enum TransmitReceiveMode
{
	TR_MODE_SINGLE_TX_SINGLE_RX = 1, // 单发单收 (自发自收)
	TR_MODE_SINGLE_TX_DOUBLE_RX = 2, // 单发双收 (一发多收)
	TR_MODE_PING_PONG           = 3, // 乒乓模式
	TR_MODE_DOUBLE_FREQ_PING    = 4  // 双频乒乓模式
};
#include"opencv2\core\core.hpp"
#include"opencv2\highgui\highgui.hpp"
#include"opencv2\imgproc\imgproc.hpp"
#include"opencv2\opencv.hpp"
#include <omp.h>  /*多线程计算库*/

/*-------------------------------------------------------*/
/*                    三维位置矢量                       */
/*-------------------------------------------------------*/
struct Position
{
	double x;
	double y;
	double z;
	Position()
	{
		this->x = 0.0;
		this->y = 0.0;
		this->z = 0.0;
	}
	/*传值构造函数*/
	Position(double x, double y, double z)
	{
		this->x = x;
		this->y = y;
		this->z = z;
	}

};

/*-------------------------------------------------------*/
/*                    三维速度矢量                       */
/*-------------------------------------------------------*/
struct Velocity
{
	double vx;
	double vy;
	double vz;
	Velocity()
	{
		this->vx = 0.0;
		this->vy = 0.0;
		this->vz = 0.0;
	}
	/*传值构造函数*/
	Velocity(double vx, double vy, double vz)
	{
		this->vx = vx;
		this->vy = vy;
		this->vz = vz;
	}

};

/*-------------------------------------------------------*/
/*                   卫星轨道信息                        */
/*-------------------------------------------------------*/
struct OSV
{
	double time;
	double x;
	double y;
	double z;
	double vx;
	double vy;
	double vz;
	OSV()
	{
		time = x = y = z = vx = vy = vz = 0.0;
	}
	OSV(double time, double x, double y, double z, double vx, double vy, double vz)
	{
		this->time = time;
		this->x = x;
		this->y = y;
		this->z = z;
		this->vx = vx;
		this->vy = vy;
		this->vz = vz;
	}

};

typedef bool (*ProgressCallback)(int percent, const char* message, void* userData);

#define InSAR_API __declspec(dllexport)
#ifdef _DEBUG

#pragma comment(lib, "opencv_world450d.lib")


#else

#pragma comment(lib, "opencv_world450.lib")


#endif // DEBUG




