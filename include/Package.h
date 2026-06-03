#pragma once
#ifndef __PACKAGE__H__
#define __PACKAGE__H__

// C4251: å¯¼å‡ºç±»çš„æˆå‘˜ä½¿ç”¨äº†æ— DLLå¯¼å‡ºæ¥å£çš„ç±»å‹(std::string, cv::Matç­‰)ï¼Œå¯¹æœ¬é¡¹ç›®æ— å®é™…å½±å“
#pragma warning(disable: 4251)
#define PI 3.141592653589793238
#define VEL_C 299792458.0
#define INPUTMAXSIZE 1024
#include"opencv2\core\core.hpp"
#include"opencv2\highgui\highgui.hpp"
#include"opencv2\imgproc\imgproc.hpp"
#include"opencv2\opencv.hpp"
#include <omp.h>  /*¶àÏß³Ì¼ÆËã¿â*/

/*-------------------------------------------------------*/
/*                    ÈıÎ¬Î»ÖÃÊ¸Á¿                       */
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
	/*´«Öµ¹¹Ôìº¯Êı*/
	Position(double x, double y, double z)
	{
		this->x = x;
		this->y = y;
		this->z = z;
	}
	/*¿½±´¹¹Ôìº¯Êı*/
	Position(const Position& cp)
	{
		this->x = cp.x;
		this->y = cp.y;
		this->z = cp.z;
	}
	/*¸³Öµº¯Êı(Éî¿½±´)*/
	Position operator=(const Position& cp)
	{
		this->x = cp.x;
		this->y = cp.y;
		this->z = cp.z;
		return *this;
	}

};

/*-------------------------------------------------------*/
/*                    ÈıÎ¬ËÙ¶ÈÊ¸Á¿                       */
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
	/*´«Öµ¹¹Ôìº¯Êı*/
	Velocity(double vx, double vy, double vz)
	{
		this->vx = vx;
		this->vy = vy;
		this->vz = vz;
	}
	/*¿½±´¹¹Ôìº¯Êı*/
	Velocity(const Velocity& cp)
	{
		this->vx = cp.vx;
		this->vy = cp.vy;
		this->vz = cp.vz;
	}
	/*¸³Öµº¯Êı(Éî¿½±´)*/
	Velocity operator=(const Velocity& cp)
	{
		this->vx = cp.vx;
		this->vy = cp.vy;
		this->vz = cp.vz;
		return *this;
	}

};

/*-------------------------------------------------------*/
/*                   ÎÀĞÇ¹ìµÀĞÅÏ¢                        */
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
	/*¿½±´¹¹Ôìº¯Êı*/
	OSV(const OSV& osv)
	{
		this->time = osv.time;
		this->x = osv.x;
		this->y = osv.y;
		this->z = osv.z;
		this->vx = osv.vx;
		this->vy = osv.vy;
		this->vz = osv.vz;
	}
	/*¸³Öµº¯Êı*/
	OSV operator=(const OSV& osv)
	{
		this->time = osv.time;
		this->x = osv.x;
		this->y = osv.y;
		this->z = osv.z;
		this->vx = osv.vx;
		this->vy = osv.vy;
		this->vz = osv.vz;
		return *this;
	}

};

#define InSAR_API __declspec(dllexport)
#ifdef _DEBUG

#pragma comment(lib, "opencv_world450d.lib")


#else

#pragma comment(lib, "opencv_world450.lib")


#endif // DEBUG


#endif // !__PACKAGE__H__

