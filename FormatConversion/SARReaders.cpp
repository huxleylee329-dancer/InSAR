#include "pch.h"
#include "..\include\FormatConversion.h"
#include "..\include\Hdf5IO.h"
#include "gdal_priv.h"
#include "tinyxml.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>

using namespace cv;
using namespace std;

namespace
{
	std::recursive_mutex g_reader_init_mutex;
	std::once_flag g_reader_gdal_init_flag;

	void InitializeGDALOnce()
	{
		std::call_once(g_reader_gdal_init_flag, []() { GDALAllRegister(); });
	}
}

#define READER_INIT_LOCK std::lock_guard<std::recursive_mutex> reader_init_lock(g_reader_init_mutex)
extern int UTC2GPS(const char* utc_time, double* gps_time);
CSK_reader::CSK_reader(const char* csk_data_file)
{
	b_initialized = false;
	this->csk_data_file = csk_data_file;
}

CSK_reader::~CSK_reader()
{
}

int CSK_reader::init()
{
	READER_INIT_LOCK;
	if (b_initialized) return 0;
	if (csk_data_file.empty())
	{
		fprintf(stderr, "init(): input check failed!\n");
		return -1;
	}
	int ret = read_data(this->csk_data_file.c_str());
	if (ret < 0)
	{
		fprintf(stderr, "init(): read_meta_data failed!\n");
		return -1;
	}
	b_initialized = true;
	return 0;
}

int CSK_reader::read_slc(const char* CSK_data_file, ComplexMat& slc)
{
	if (CSK_data_file == NULL)
	{
		fprintf(stderr, "read_slc(): input check  failed!\n");
		return -1;
	}
	std::unique_ptr<Hdf5IO::ReadSession, void(*)(Hdf5IO::ReadSession*)> session(
		Hdf5IO::openReadSession(CSK_data_file), Hdf5IO::closeReadSession);
	if (!session)
	{
		fprintf(stderr, "read_slc(): failed to open %s!\n", CSK_data_file);
		return -1;
	}
	if (Hdf5IO::readInterleavedComplexFloat(session.get(), "/S01/IMG", slc.re, slc.im) != 0)
	{
		fprintf(stderr, "read_slc(): failed to read from /S01/IMG!\n");
		return -1;
	}
	return 0;
}

int CSK_reader::read_data(const char* CSK_data_file)
{
	if (!CSK_data_file)
	{
		fprintf(stderr, "read_data(): input check failed!\n");
		return -1;
	}
	std::unique_ptr<Hdf5IO::ReadSession, void(*)(Hdf5IO::ReadSession*)> session(
		Hdf5IO::openReadSession(CSK_data_file), Hdf5IO::closeReadSession);
	if (!session)
	{
		fprintf(stderr, "read_data(): can't open %s\n", CSK_data_file);
		return -1;
	}
	if (Hdf5IO::readInterleavedComplexFloat(session.get(), "/S01/IMG", slc.re, slc.im) != 0)
	{
		fprintf(stderr, "read_data(): can't read slc from %s\n", CSK_data_file);
		return -1;
	}
	
	string Reference_UTC;
	double start, stop, ref_time;
	int ret = Hdf5IO::readStringAttribute(session.get(), nullptr, "Reference UTC", Reference_UTC);
	if (ret < 0)
	{
		fprintf(stderr, "read_meta_data(): can't read Reference UTC\n");
		return -1;
	}
	std::replace(Reference_UTC.begin(), Reference_UTC.end(), ' ', 'T');
	UTC2GPS(Reference_UTC.c_str(), &ref_time);
	Mat vel, pos, state_vectors_times; Mat temp;
	ret = Hdf5IO::readArrayAttribute(session.get(), nullptr, "ECEF Satellite Position", pos);
	ret = Hdf5IO::readArrayAttribute(session.get(), nullptr, "ECEF Satellite Velocity", vel);
	ret = Hdf5IO::readArrayAttribute(session.get(), nullptr, "State Vectors Times", state_vectors_times);
	state_vectors_times = state_vectors_times + ref_time;
	cv::hconcat(state_vectors_times, pos, this->state_vec);
	cv::hconcat(this->state_vec, vel, this->state_vec);
	Hdf5IO::readArrayAttribute(session.get(), nullptr, "Radar Frequency", temp);
	this->carrier_frequency = temp.at<double>(0, 0);
	Hdf5IO::readStringAttribute(session.get(), nullptr, "Look Side", lookside);
	Hdf5IO::readStringAttribute(session.get(), nullptr, "Mission ID", sensor);
	Hdf5IO::readStringAttribute(session.get(), nullptr, "Polarization", polarization);
	Hdf5IO::readStringAttribute(session.get(), nullptr, "Orbit Direction", orbit_direction);
	
	int rows, cols;
	Hdf5IO::readArrayAttribute(session.get(), "/S01/IMG", "Zero Doppler Azimuth First Time", temp);
	start = temp.at<double>(0, 0);
	string front = Reference_UTC.substr(0, 17);
	char str[256];
	sprintf(str, "%.9lf", temp.at<double>(0, 0));
	this->acquisition_start_time = front + str;

	Hdf5IO::readArrayAttribute(session.get(), "/S01/IMG", "Zero Doppler Azimuth Last Time", temp);
	stop = temp.at<double>(0, 0);
	front = Reference_UTC.substr(0, 17);
	sprintf(str, "%.9lf", temp.at<double>(0, 0));
	this->acquisition_stop_time = front + str;

	Hdf5IO::readArrayAttribute(session.get(), "/S01/IMG", "Zero Doppler Range First Time", temp);
	this->slant_range_first_pixel = temp.at<double>(0, 0) * VEL_C / 2.0;

	Hdf5IO::readArrayAttribute(session.get(), "/S01/IMG", "Zero Doppler Range Last Time", temp);
	this->slant_range_last_pixel = temp.at<double>(0, 0) * VEL_C / 2.0;

	Hdf5IO::readArrayAttribute(session.get(), "/S01/IMG", "Column Spacing", temp);
	this->range_spacing = temp.at<double>(0, 0);

	Hdf5IO::readArrayAttribute(session.get(), "/S01/IMG", "Line Spacing", temp);
	this->azimuth_spacing = temp.at<double>(0, 0);

	Hdf5IO::readArrayAttribute(session.get(), "/S01/IMG", "Line Time Interval", temp);
	this->prf = 1.0 / temp.at<double>(0, 0);

	rows = (int)round((stop - start) * prf) + 1;
	cols = (int)round((slant_range_last_pixel - slant_range_first_pixel) / range_spacing) + 1;

	Hdf5IO::readArrayAttribute(session.get(), "/S01/IMG", "Bottom Left Geodetic Coordinates", bottomleft);
	Hdf5IO::readArrayAttribute(session.get(), "/S01/IMG", "Bottom Right Geodetic Coordinates", bottomright);
	Hdf5IO::readArrayAttribute(session.get(), "/S01/IMG", "Top Left Geodetic Coordinates", topleft);
	Hdf5IO::readArrayAttribute(session.get(), "/S01/IMG", "Top Right Geodetic Coordinates", topright);

	double near_look_angle, far_look_angle;
	Hdf5IO::readArrayAttribute(session.get(), "/S01/IMG", "Near Look Angle", temp);
	near_look_angle = temp.at<double>(0, 0);
	Hdf5IO::readArrayAttribute(session.get(), "/S01/IMG", "Far Look Angle", temp);
	far_look_angle = temp.at<double>(0, 0);

	//拟合下视角
	
	this->inc_coefficient.create(1, 11, CV_64F); this->inc_coefficient = 0.0;
	inc_coefficient.at<double>(0, 0) = 0.0;//offset
	inc_coefficient.at<double>(0, 1) = 1.0;//scale
	inc_coefficient.at<double>(0, 2) = near_look_angle;
	inc_coefficient.at<double>(0, 3) = (far_look_angle - near_look_angle) / (double)cols;

	//拟合经度
	Mat near_edge_geodetic_coordinates, far_edge_geodetic_coordinates;
	ret = Hdf5IO::readArrayAttribute(session.get(), nullptr, "Scene Far Edge Geodetic Coordinates", far_edge_geodetic_coordinates);
	ret += Hdf5IO::readArrayAttribute(session.get(), nullptr, "Scene Near Edge Geodetic Coordinates", near_edge_geodetic_coordinates);
	if (ret != 0)
	{
		fprintf(stderr, "read_data(): unknown format!\n");
		return -1;
	}
	Mat lon, lat, row, col;
	lon = Mat::zeros(far_edge_geodetic_coordinates.rows + near_edge_geodetic_coordinates.rows, 1, CV_64F);
	lat = Mat::zeros(lon.rows, 1, CV_64F);
	col = Mat::zeros(lon.rows, 1, CV_64F);
	row = Mat::zeros(lon.rows, 1, CV_64F);
	cv::vconcat(far_edge_geodetic_coordinates, near_edge_geodetic_coordinates, temp);
	temp(cv::Range(0, lon.rows), cv::Range(0, 1)).copyTo(lat);
	temp(cv::Range(0, lon.rows), cv::Range(1, 2)).copyTo(lon);
	for (int i = 0; i < far_edge_geodetic_coordinates.rows; i++)
	{
		row.at<double>(i, 0) = i * rows / (double)(far_edge_geodetic_coordinates.rows);
		col.at<double>(i, 0) = cols;
	}
	for (int i = 0; i < far_edge_geodetic_coordinates.rows; i++)
	{
		row.at<double>(i + far_edge_geodetic_coordinates.rows, 0) = i * rows / (double)(far_edge_geodetic_coordinates.rows);
		col.at<double>(i + far_edge_geodetic_coordinates.rows, 0) = 0;
	}
	lon_coefficient.create(1, 32, CV_64F); lon_coefficient = 0.0;
	Mat A(lon.rows, 3, CV_64F); A = 1.0;
	Mat A_t, b;
	Mat B, coefficient, error, eye, b_t, a, a_t;
	double rms;
	lon.copyTo(b);
	row.copyTo(A(cv::Range(0, lon.rows), cv::Range(1, 2)));
	col.copyTo(A(cv::Range(0, lon.rows), cv::Range(2, 3)));

	cv::transpose(A, temp);
	B = temp * b;
	A.copyTo(a);
	cv::transpose(a, a_t);
	A = temp * A;
	rms = -1.0;
	if (cv::invert(A, error, cv::DECOMP_LU) > 0)
	{
		cv::transpose(b, b_t);
		error = b_t * b - (b_t * a) * error * (a_t * b);
		//error = b_t * (eye - a * error * a_t) * b;
		rms = sqrt(error.at<double>(0, 0) / double(b.rows));
	}
	if (cv::solve(A, B, coefficient, cv::DECOMP_NORMAL))
	{
		lon_coefficient.at<double>(0, 0) = 0.0;
		lon_coefficient.at<double>(0, 1) = 1.0;
		lon_coefficient.at<double>(0, 2) = 0.0;
		lon_coefficient.at<double>(0, 3) = 1.0;
		lon_coefficient.at<double>(0, 4) = 0.0;
		lon_coefficient.at<double>(0, 5) = 1.0;
		lon_coefficient.at<double>(0, 31) = rms;

		lon_coefficient.at<double>(0, 6) = coefficient.at<double>(0);
		lon_coefficient.at<double>(0, 7) = coefficient.at<double>(1);
		lon_coefficient.at<double>(0, 11) = coefficient.at<double>(2);
	}


	//拟合纬度
	lat_coefficient.create(1, 32, CV_64F); lat_coefficient = 0.0;
	A = Mat::ones(lon.rows, 3, CV_64F);
	lat.copyTo(b);
	row.copyTo(A(cv::Range(0, lon.rows), cv::Range(1, 2)));
	col.copyTo(A(cv::Range(0, lon.rows), cv::Range(2, 3)));

	cv::transpose(A, temp);
	B = temp * b;
	A.copyTo(a);
	cv::transpose(a, a_t);
	A = temp * A;
	rms = -1.0;
	if (cv::invert(A, error, cv::DECOMP_LU) > 0)
	{
		cv::transpose(b, b_t);
		error = b_t * b - (b_t * a) * error * (a_t * b);
		rms = sqrt(error.at<double>(0, 0) / double(b.rows));
	}
	if (cv::solve(A, B, coefficient, cv::DECOMP_NORMAL))
	{
		lat_coefficient.at<double>(0, 0) = 0.0;
		lat_coefficient.at<double>(0, 1) = 1.0;
		lat_coefficient.at<double>(0, 2) = 0.0;
		lat_coefficient.at<double>(0, 3) = 1.0;
		lat_coefficient.at<double>(0, 4) = 0.0;
		lat_coefficient.at<double>(0, 5) = 1.0;
		lat_coefficient.at<double>(0, 31) = rms;

		lat_coefficient.at<double>(0, 6) = coefficient.at<double>(0);
		lat_coefficient.at<double>(0, 7) = coefficient.at<double>(1);
		lat_coefficient.at<double>(0, 11) = coefficient.at<double>(2);
	}

	//拟合行坐标
	row_coefficient.create(1, 32, CV_64F); row_coefficient = 0.0;
	A = Mat::ones(lon.rows, 3, CV_64F);
	row.copyTo(b);
	b = b / (double)rows;
	lon.copyTo(A(cv::Range(0, lon.rows), cv::Range(1, 2)));
	lat.copyTo(A(cv::Range(0, lon.rows), cv::Range(2, 3)));

	cv::transpose(A, temp);
	B = temp * b;
	A.copyTo(a);
	cv::transpose(a, a_t);
	A = temp * A;
	rms = -1.0;
	if (cv::invert(A, error, cv::DECOMP_LU) > 0)
	{
		cv::transpose(b, b_t);
		error = b_t * b - (b_t * a) * error * (a_t * b);
		rms = sqrt(error.at<double>(0, 0) / double(b.rows));
	}
	if (cv::solve(A, B, coefficient, cv::DECOMP_NORMAL))
	{
		row_coefficient.at<double>(0, 0) = 0.0;
		row_coefficient.at<double>(0, 1) = rows;
		row_coefficient.at<double>(0, 2) = 0.0;
		row_coefficient.at<double>(0, 3) = 1.0;
		row_coefficient.at<double>(0, 4) = 0.0;
		row_coefficient.at<double>(0, 5) = 1.0;
		row_coefficient.at<double>(0, 31) = rms;

		row_coefficient.at<double>(0, 6) = coefficient.at<double>(0);
		row_coefficient.at<double>(0, 7) = coefficient.at<double>(1);
		row_coefficient.at<double>(0, 11) = coefficient.at<double>(2);
	}

	//拟合列坐标
	col_coefficient.create(1, 32, CV_64F); col_coefficient = 0.0;
	A = Mat::ones(lon.rows, 3, CV_64F);
	col.copyTo(b);
	b = b / (double)cols;
	lon.copyTo(A(cv::Range(0, lon.rows), cv::Range(1, 2)));
	lat.copyTo(A(cv::Range(0, lon.rows), cv::Range(2, 3)));

	cv::transpose(A, temp);
	B = temp * b;
	A.copyTo(a);
	cv::transpose(a, a_t);
	A = temp * A;
	rms = -1.0;
	if (cv::invert(A, error, cv::DECOMP_LU) > 0)
	{
		cv::transpose(b, b_t);
		error = b_t * b - (b_t * a) * error * (a_t * b);
		rms = sqrt(error.at<double>(0, 0) / double(b.rows));
	}
	if (cv::solve(A, B, coefficient, cv::DECOMP_NORMAL))
	{
		col_coefficient.at<double>(0, 0) = 0.0;
		col_coefficient.at<double>(0, 1) = cols;
		col_coefficient.at<double>(0, 2) = 0.0;
		col_coefficient.at<double>(0, 3) = 1.0;
		col_coefficient.at<double>(0, 4) = 0.0;
		col_coefficient.at<double>(0, 5) = 1.0;
		col_coefficient.at<double>(0, 31) = rms;

		col_coefficient.at<double>(0, 6) = coefficient.at<double>(0);
		col_coefficient.at<double>(0, 7) = coefficient.at<double>(1);
		col_coefficient.at<double>(0, 11) = coefficient.at<double>(2);
	}



	return 0;
}

int CSK_reader::write_custom_h5_data(FormatConversion& conversion, const char* dst_h5)
{
	conversion.write_array_to_h5(dst_h5, "col_coefficient", this->col_coefficient);
	conversion.write_array_to_h5(dst_h5, "row_coefficient", this->row_coefficient);
	conversion.write_array_to_h5(dst_h5, "lon_coefficient", this->lon_coefficient);
	conversion.write_array_to_h5(dst_h5, "lat_coefficient", this->lat_coefficient);
	conversion.write_array_to_h5(dst_h5, "inc_coefficient", this->inc_coefficient);

	conversion.write_double_to_h5(dst_h5, "slant_range_last_pixel", this->slant_range_last_pixel);

	conversion.write_str_to_h5(dst_h5, "orbit_dir", this->orbit_direction.c_str());
	conversion.write_str_to_h5(dst_h5, "polarization", this->polarization.c_str());
	conversion.write_str_to_h5(dst_h5, "lookside", this->lookside.c_str());

	return 0;
}

HTHT_reader::HTHT_reader(const char* data_file, const char* xml_file, int mode)
{
	b_initialized = false;
	this->HT_data_file = data_file;
	this->HT_xml_file = xml_file;
	this->mode = mode;
}

HTHT_reader::~HTHT_reader()
{
}

int HTHT_reader::init()
{
	if (b_initialized) return 0;
	if (HT_data_file.empty())
	{
		fprintf(stderr, "init(): input check failed!\n");
		return -1;
	}
	int ret = read_data(this->HT_xml_file.c_str(), this->HT_data_file.c_str());
	if (ret < 0)
	{
		fprintf(stderr, "init(): read_data failed!\n");
		return -1;
	}
	b_initialized = true;
	return 0;
}

int HTHT_reader::read_slc(const char* data_file, ComplexMat& slc)
{
	if (data_file == NULL)
	{
		fprintf(stderr, "read_slc(): input check failed!\n");
		return -1;
	}

	InitializeGDALOnce();

	GDALDatasetH hDataset = GDALOpen(data_file, GA_ReadOnly);
	if (hDataset == NULL)
	{
		fprintf(stderr, "read_slc(): failed to open %s!\n", data_file);
		return -1;
	}

	int nBand = GDALGetRasterCount(hDataset);
	if (nBand < 2)
	{
		fprintf(stderr, "read_slc(): number of bands < 2!\n");
		GDALClose(hDataset);
		return -1;
	}

	/* ---------- Band 1: Real ---------- */
	GDALRasterBandH hBand = GDALGetRasterBand(hDataset, 1);
	if (hBand == NULL)
	{
		fprintf(stderr, "read_slc(): failed to get band 1!\n");
		GDALClose(hDataset);
		return -1;
	}

	int xsize = GDALGetRasterBandXSize(hBand);
	int ysize = GDALGetRasterBandYSize(hBand);
	if (xsize <= 0 || ysize <= 0)
	{
		fprintf(stderr, "read_slc(): band rows and cols error!\n");
		GDALClose(hDataset);
		return -1;
	}

	short* pbuf = (short*)malloc(sizeof(short) * xsize * ysize);
	if (!pbuf)
	{
		fprintf(stderr, "read_slc(): out of memory!\n");
		GDALClose(hDataset);
		return -1;
	}

	if (GDALRasterIO(
		hBand,
		GF_Read,
		0, 0,
		xsize, ysize,
		pbuf,
		xsize, ysize,
		GDT_Int16,
		0, 0) != CE_None)
	{
		fprintf(stderr, "read_slc(): RasterIO failed on band 1!\n");
		free(pbuf);
		GDALClose(hDataset);
		return -1;
	}

	slc.re.create(ysize, xsize, CV_16S);

	size_t offset = 0;
	for (int i = 0; i < ysize; i++)
	{
		short* rowp = slc.re.ptr<short>(i);
		for (int j = 0; j < xsize; j++)
			rowp[j] = pbuf[offset++];
	}

	/* ---------- Band 2: Imag ---------- */
	hBand = GDALGetRasterBand(hDataset, 2);
	if (hBand == NULL)
	{
		fprintf(stderr, "read_slc(): failed to get band 2!\n");
		free(pbuf);
		GDALClose(hDataset);
		return -1;
	}

	if (GDALRasterIO(
		hBand,
		GF_Read,
		0, 0,
		xsize, ysize,
		pbuf,
		xsize, ysize,
		GDT_Int16,
		0, 0) != CE_None)
	{
		fprintf(stderr, "read_slc(): RasterIO failed on band 2!\n");
		free(pbuf);
		GDALClose(hDataset);
		return -1;
	}

	slc.im.create(ysize, xsize, CV_16S);

	offset = 0;
	for (int i = 0; i < ysize; i++)
	{
		short* rowp = slc.im.ptr<short>(i);
		for (int j = 0; j < xsize; j++)
			rowp[j] = pbuf[offset++];
	}

	free(pbuf);
	GDALClose(hDataset);

	return 0;
}

int HTHT_reader::read_data(const char* xml_file, const char* data_file)
{
	if (!xml_file || !data_file)
	{
		fprintf(stderr, "read_data(): input check failed!\n");
		return -1;
	}
	int ret = read_slc(data_file, slc);
	if (ret < 0)
	{
		fprintf(stderr, "read_data(): can't read slc from %s\n", data_file);
		return -1;
	}
	XMLFile xmldoc;
	ret = xmldoc.XMLFile_load(xml_file);
	if (ret < 0)
	{
		fprintf(stderr, "read_data(): can't load %s\n", xml_file);
		return -1;
	}

	//读取轨道参数
	TiXmlElement* pnode, * pchild;
	// removed unused: pchild1 (copy-paste remnant from similar XML parsing)
	int numOfstateVec;
	ret = xmldoc.find_node("orbitList", pnode);
	ret = sscanf(pnode->FirstAttribute()->Value(), "%d", &numOfstateVec);

	ret = xmldoc.find_node("orbit", pnode);
	double time, x, y, z, vx, vy, vz;
	state_vec.create(numOfstateVec, 7, CV_64F);
	for (int i = 0; i < numOfstateVec; i++)
	{
		if (!pnode) break;
		//GPS时间
		ret = xmldoc._find_node(pnode, "timeStamp", pchild);
		ret = UTC2GPS(pchild->GetText(), &time);
		//位置x
		ret = xmldoc._find_node(pnode, "xPosition", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &x);
		//位置y
		ret = xmldoc._find_node(pnode, "yPosition", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &y);
		//位置z
		ret = xmldoc._find_node(pnode, "zPosition", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &z);
		//速度x
		ret = xmldoc._find_node(pnode, "xVelocity", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &vx);
		//速度y
		ret = xmldoc._find_node(pnode, "yVelocity", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &vy);
		//速度z
		ret = xmldoc._find_node(pnode, "zVelocity", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &vz);

		//赋值
		state_vec.at<double>(i, 0) = time;
		state_vec.at<double>(i, 1) = x;
		state_vec.at<double>(i, 2) = y;
		state_vec.at<double>(i, 3) = z;
		state_vec.at<double>(i, 4) = vx;
		state_vec.at<double>(i, 5) = vy;
		state_vec.at<double>(i, 6) = vz;
		pnode = pnode->NextSiblingElement();
	}

	//拍摄起始时间
	ret = xmldoc.get_str_para("imagingStartTime", this->acquisition_start_time);
	//拍摄结束时间
	ret = xmldoc.get_str_para("imagingEndTime", this->acquisition_stop_time);
	//卫星名称
	ret = xmldoc.get_str_para("satellite", this->sensor);
	//脉冲重复频率
	ret = xmldoc.get_double_para("prf", &this->prf);
	//中心频率
	ret = xmldoc.get_double_para("radarCenterFrequency", &this->carrier_frequency);
	this->carrier_frequency = this->carrier_frequency * 1e9;
	//最近斜距
	ret = xmldoc.get_double_para("nearRange", &this->slant_range_first_pixel);
	//距离方位采样间隔/分辨率
	ret = xmldoc.get_double_para("rangePixelSpacing", &this->range_spacing);
	ret = xmldoc.get_double_para("azimuthPixelSpacing", &this->azimuth_spacing);
	ret = xmldoc.get_double_para("rangeResolution", &this->range_resolution);
	ret = xmldoc.get_double_para("azimuthResolution", &this->azimuth_resolution);

	//中心下视角incidenceAngleMidSwath
	ret = xmldoc.get_double_para("incidenceCenter", &this->inc_center);

	//四角经纬度
	ret = xmldoc.get_double_para("topLeftLat", &this->topleft_lat);
	ret = xmldoc.get_double_para("topLeftLon", &this->topleft_lon);
	ret = xmldoc.get_double_para("topRightLat", &this->topright_lat);
	ret = xmldoc.get_double_para("topRightLon", &this->topright_lon);
	ret = xmldoc.get_double_para("bottomLeftLat", &this->bottomleft_lat);
	ret = xmldoc.get_double_para("bottomLeftLon", &this->bottomleft_lon);
	ret = xmldoc.get_double_para("bottomRightLat", &this->bottomright_lat);
	ret = xmldoc.get_double_para("bottomRightLon", &this->bottomright_lon);

	return 0;
}

int HTHT_reader::write_custom_h5_data(FormatConversion& conversion, const char* dst_h5)
{
	write_common_coordinates(conversion, dst_h5);
	conversion.write_int_to_h5(dst_h5, "TR_mode", this->mode + 1);
	return 0;
}

AIRSAT_reader::AIRSAT_reader(const char* data_file, const char* xml_file)
{
	b_initialized = false;
	this->AIRSAT_data_file = data_file;
	this->AIRSAT_xml_file = xml_file;
}

AIRSAT_reader::~AIRSAT_reader()
{
}

int AIRSAT_reader::init()
{
	if (b_initialized) return 0;
	if (AIRSAT_data_file.empty())
	{
		fprintf(stderr, "init(): input check failed!\n");
		return -1;
	}
	int ret = read_data(this->AIRSAT_xml_file.c_str(), this->AIRSAT_data_file.c_str());
	if (ret < 0)
	{
		fprintf(stderr, "init(): read_data failed!\n");
		return -1;
	}
	b_initialized = true;
	return 0;
}

int AIRSAT_reader::UTC2GPS(const char* utc_time, double* gps_time)
{
	if (utc_time == NULL || gps_time == NULL)
	{
		fprintf(stderr, "UTC2GPS(): input check failed!\n");
		return -1;
	}
	int ret, year, month, day, hour, minute, second/*, s*/;
	double sec;
	ret = sscanf(utc_time, "%d-%d-%d %d:%d:%lf\n", &year, &month, &day, &hour, &minute, &sec);
	if (ret != 6)
	{
		fprintf(stderr, "UTC2GPS(): %s: unknown format!\n", utc_time);
		return -1;
	}
	second = int(floor(sec));
	sec = sec - (double)second;
	tm TM;
	TM.tm_year = year - 1900;
	TM.tm_mon = month - 1;
	TM.tm_mday = day;
	TM.tm_hour = hour;
	TM.tm_min = minute;
	TM.tm_sec = second;
	TM.tm_isdst = 0;
	*gps_time = double(mktime(&TM) - 315964809) + sec;
	return 0;
}

int AIRSAT_reader::read_slc(const char* data_file, ComplexMat& slc)
{
	if (data_file == NULL)
	{
		fprintf(stderr, "read_slc(): input check failed!\n");
		return -1;
	}

	InitializeGDALOnce();

	GDALDatasetH hDataset = GDALOpen(data_file, GA_ReadOnly);
	if (hDataset == NULL)
	{
		fprintf(stderr, "read_slc(): failed to open %s!\n", data_file);
		return -1;
	}

	int nBand = GDALGetRasterCount(hDataset);
	if (nBand < 2)
	{
		fprintf(stderr, "read_slc(): number of bands < 2!\n");
		GDALClose(hDataset);
		return -1;
	}

	/* ================= Band 1 : Real ================= */
	GDALRasterBandH hBand = GDALGetRasterBand(hDataset, 1);
	if (hBand == NULL)
	{
		fprintf(stderr, "read_slc(): failed to get band 1!\n");
		GDALClose(hDataset);
		return -1;
	}

	int xsize = GDALGetRasterBandXSize(hBand);
	int ysize = GDALGetRasterBandYSize(hBand);
	if (xsize <= 0 || ysize <= 0)
	{
		fprintf(stderr, "read_slc(): band rows and cols error!\n");
		GDALClose(hDataset);
		return -1;
	}

	short* pbuf = (short*)malloc(sizeof(short) * xsize * ysize);
	if (!pbuf)
	{
		fprintf(stderr, "read_slc(): out of memory!\n");
		GDALClose(hDataset);
		return -1;
	}

	if (GDALRasterIO(
		hBand,
		GF_Read,
		0, 0,
		xsize, ysize,
		pbuf,
		xsize, ysize,
		GDT_Int16,
		0, 0) != CE_None)
	{
		fprintf(stderr, "read_slc(): RasterIO failed on band 1!\n");
		free(pbuf);
		GDALClose(hDataset);
		return -1;
	}

	slc.re.create(ysize, xsize, CV_16S);

	size_t offset = 0;
	for (int i = 0; i < ysize; i++)
	{
		short* rowp = slc.re.ptr<short>(i);
		for (int j = 0; j < xsize; j++)
			rowp[j] = pbuf[offset++];
	}

	/* ================= Band 2 : Imag ================= */
	hBand = GDALGetRasterBand(hDataset, 2);
	if (hBand == NULL)
	{
		fprintf(stderr, "read_slc(): failed to get band 2!\n");
		free(pbuf);
		GDALClose(hDataset);
		return -1;
	}

	if (GDALRasterIO(
		hBand,
		GF_Read,
		0, 0,
		xsize, ysize,
		pbuf,
		xsize, ysize,
		GDT_Int16,
		0, 0) != CE_None)
	{
		fprintf(stderr, "read_slc(): RasterIO failed on band 2!\n");
		free(pbuf);
		GDALClose(hDataset);
		return -1;
	}

	slc.im.create(ysize, xsize, CV_16S);

	offset = 0;
	for (int i = 0; i < ysize; i++)
	{
		short* rowp = slc.im.ptr<short>(i);
		for (int j = 0; j < xsize; j++)
			rowp[j] = pbuf[offset++];
	}

	free(pbuf);
	GDALClose(hDataset);

	return 0;
}

int AIRSAT_reader::write_custom_h5_data(FormatConversion& conversion, const char* dst_h5)
{
	write_common_coordinates(conversion, dst_h5);
	return 0;
}

int AIRSAT_reader::read_data(const char* xml_file, const char* data_file)
{
	if (!xml_file || !data_file)
	{
		fprintf(stderr, "read_data(): input check failed!\n");
		return -1;
	}
	int ret = read_slc(data_file, slc);
	if (ret < 0)
	{
		fprintf(stderr, "read_data(): can't read slc from %s\n", data_file);
		return -1;
	}
	XMLFile xmldoc;
	ret = xmldoc.XMLFile_load(xml_file);
	if (ret < 0)
	{
		fprintf(stderr, "read_data(): can't load %s\n", xml_file);
		return -1;
	}

	//读取轨道参数
	TiXmlElement* pnode, * pchild/*, * pchild1*/;
	int numOfstateVec = 0;
	pnode = NULL;
	ret = xmldoc.find_node("GPSParam", pnode);
	while (pnode)
	{
		numOfstateVec += 1;
		pnode = pnode->NextSiblingElement();
	}

	ret = xmldoc.find_node("GPSParam", pnode);
	double time, x, y, z, vx, vy, vz;
	state_vec.create(numOfstateVec, 7, CV_64F);
	for (int i = 0; i < numOfstateVec; i++)
	{
		if (!pnode) break;
		//GPS时间
		ret = xmldoc._find_node(pnode, "TimeStamp", pchild);
		ret = this->UTC2GPS(pchild->GetText(), &time);
		//位置x
		ret = xmldoc._find_node(pnode, "xPosition", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &x);
		//位置y
		ret = xmldoc._find_node(pnode, "yPosition", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &y);
		//位置z
		ret = xmldoc._find_node(pnode, "zPosition", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &z);
		//速度x
		ret = xmldoc._find_node(pnode, "xVelocity", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &vx);
		//速度y
		ret = xmldoc._find_node(pnode, "yVelocity", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &vy);
		//速度z
		ret = xmldoc._find_node(pnode, "zVelocity", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &vz);

		//赋值
		state_vec.at<double>(i, 0) = time;
		state_vec.at<double>(i, 1) = x;
		state_vec.at<double>(i, 2) = y;
		state_vec.at<double>(i, 3) = z;
		state_vec.at<double>(i, 4) = vx;
		state_vec.at<double>(i, 5) = vy;
		state_vec.at<double>(i, 6) = vz;
		pnode = pnode->NextSiblingElement();
	}

	//拍摄起始时间
	ret = xmldoc.find_node("imagingTime", pnode);
	ret = xmldoc._find_node(pnode, "start", pchild);
	this->acquisition_start_time = pchild->GetText();
	std::replace(this->acquisition_start_time.begin(), this->acquisition_start_time.end(), ' ', 'T');
	//拍摄结束时间
	ret = xmldoc._find_node(pnode, "end", pchild);
	this->acquisition_stop_time = pchild->GetText();
	std::replace(this->acquisition_stop_time.begin(), this->acquisition_stop_time.end(), ' ', 'T');
	//卫星名称
	ret = xmldoc.get_str_para("satellite", this->sensor);
	//脉冲重复频率
	ret = xmldoc.get_double_para("Prf", &this->prf);
	//中心频率
	ret = xmldoc.get_double_para("RadarCenterFrequency", &this->carrier_frequency);
	this->carrier_frequency = this->carrier_frequency * 1e9;
	//最近斜距
	ret = xmldoc.get_double_para("nearRange", &this->slant_range_first_pixel);
	//距离方位采样间隔/分辨率
	ret = xmldoc.get_double_para("Widthspace", &this->range_spacing);
	ret = xmldoc.get_double_para("Heightspace", &this->azimuth_spacing);
	ret = xmldoc.get_double_para("Widthspace", &this->range_resolution);
	ret = xmldoc.get_double_para("Heightspace", &this->azimuth_resolution);

	//中心下视角incidenceAngleMidSwath
	double inc_near = 0, inc_far = 0;
	ret = xmldoc.get_double_para("incidenceAngleNearRange", &inc_near);
	ret = xmldoc.get_double_para("incidenceAngleFarRange", &inc_far);
	this->inc_center = (inc_near + inc_far) * 0.5;

	//四角经纬度
	ret = xmldoc.find_node("topLeft", pnode);
	ret = xmldoc._find_node(pnode, "Latitude", pchild);
	ret = sscanf(pchild->GetText(), "%lf", &this->topleft_lat);
	ret = xmldoc._find_node(pnode, "Longitude", pchild);
	ret = sscanf(pchild->GetText(), "%lf", &this->topleft_lon);

	ret = xmldoc.find_node("topRight", pnode);
	ret = xmldoc._find_node(pnode, "Latitude", pchild);
	ret = sscanf(pchild->GetText(), "%lf", &this->topright_lat);
	ret = xmldoc._find_node(pnode, "Longitude", pchild);
	ret = sscanf(pchild->GetText(), "%lf", &this->topright_lon);

	ret = xmldoc.find_node("bottomLeft", pnode);
	ret = xmldoc._find_node(pnode, "Latitude", pchild);
	ret = sscanf(pchild->GetText(), "%lf", &this->bottomleft_lat);
	ret = xmldoc._find_node(pnode, "Longitude", pchild);
	ret = sscanf(pchild->GetText(), "%lf", &this->bottomleft_lon);

	ret = xmldoc.find_node("bottomRight", pnode);
	ret = xmldoc._find_node(pnode, "Latitude", pchild);
	ret = sscanf(pchild->GetText(), "%lf", &this->bottomright_lat);
	ret = xmldoc._find_node(pnode, "Longitude", pchild);
	ret = sscanf(pchild->GetText(), "%lf", &this->bottomright_lon);

	return 0;
}


Biomass1A_reader::Biomass1A_reader(
	const char* amp_file,
	const char* phase_file,
	const char* xml_file, 
	const char* orbit_file, 
	const char* polarization
)
{
	b_initialized = false;
	this->Biomass1A_reader_amp_file = amp_file;
	this->Biomass1A_reader_phase_file = phase_file;
	this->Biomass1A_reader_orbit_file = orbit_file;
	this->Biomass1A_reader_xml_file = xml_file;
	this->polarization = polarization;
}

Biomass1A_reader::~Biomass1A_reader()
{
}

int Biomass1A_reader::init()
{
	if (b_initialized) return 0;
	if (Biomass1A_reader_amp_file.empty() || Biomass1A_reader_phase_file.empty() || Biomass1A_reader_orbit_file.empty())
	{
		fprintf(stderr, "init(): input check failed!\n");
		return -1;
	}
	int ret = read_data(this->Biomass1A_reader_xml_file.c_str(), this->Biomass1A_reader_amp_file.c_str(),
		this->Biomass1A_reader_phase_file.c_str(), this->Biomass1A_reader_orbit_file.c_str());
	if (ret < 0)
	{
		fprintf(stderr, "init(): read_data failed!\n");
		return -1;
	}
	b_initialized = true;
	return 0;
}

int Biomass1A_reader::UTC2GPS(const char* utc_time, double* gps_time)
{
	if (utc_time == NULL || gps_time == NULL)
	{
		fprintf(stderr, "UTC2GPS(): input check failed!\n");
		return -1;
	}
	int ret, year, month, day, hour, minute, second/*, s*/;
	double sec;
	ret = sscanf(utc_time, "UTC=%d-%d-%dT%d:%d:%lf\n", &year, &month, &day, &hour, &minute, &sec);
	if (ret != 6)
	{
		fprintf(stderr, "UTC2GPS(): %s: unknown format!\n", utc_time);
		return -1;
	}
	second = int(floor(sec));
	sec = sec - (double)second;
	tm TM;
	TM.tm_year = year - 1900;
	TM.tm_mon = month - 1;
	TM.tm_mday = day;
	TM.tm_hour = hour;
	TM.tm_min = minute;
	TM.tm_sec = second;
	TM.tm_isdst = 0;
	*gps_time = double(mktime(&TM) - 315964809) + sec;
	return 0;
}

int Biomass1A_reader::read_slc(
	const char* amp_file,
	const char* phase_file,
	ComplexMat& slc)
{
	if (amp_file == NULL || phase_file == NULL)
	{
		fprintf(stderr, "read_slc(): input check failed!\n");
		return -1;
	}
	int ix = 1;
	if (this->polarization == "HH") ix = 1;
	else if (this->polarization == "HV") ix = 2;
	else if (this->polarization == "VH") ix = 3;
	else  ix = 4;
	InitializeGDALOnce();

	/* ===================== 读取幅度 ===================== */
	GDALDatasetH hDS_amp = GDALOpen(amp_file, GA_ReadOnly);
	if (hDS_amp == NULL)
	{
		fprintf(stderr, "read_slc(): failed to open %s!\n", amp_file);
		return -1;
	}

	GDALRasterBandH hBand_amp = GDALGetRasterBand(hDS_amp, ix);
	if (hBand_amp == NULL)
	{
		fprintf(stderr, "read_slc(): failed to get amplitude band!\n");
		GDALClose(hDS_amp);
		return -1;
	}

	int xsize = GDALGetRasterBandXSize(hBand_amp);
	int ysize = GDALGetRasterBandYSize(hBand_amp);

	if (xsize <= 0 || ysize <= 0)
	{
		fprintf(stderr, "read_slc(): band rows and cols error!\n");
		GDALClose(hDS_amp);
		return -1;
	}

	GDALDataType dataType = GDALGetRasterDataType(hBand_amp);

	float* pbuf = (float*)malloc(sizeof(float) * xsize * ysize);
	if (!pbuf)
	{
		fprintf(stderr, "read_slc(): out of memory!\n");
		GDALClose(hDS_amp);
		return -1;
	}

	if (GDALRasterIO(
		hBand_amp,
		GF_Read,
		0, 0,
		xsize, ysize,
		pbuf,
		xsize, ysize,
		dataType,
		0, 0) != CE_None)
	{
		fprintf(stderr, "read_slc(): RasterIO (amplitude) failed!\n");
		free(pbuf);
		GDALClose(hDS_amp);
		return -1;
	}

	cv::Mat amplitude(ysize, xsize, CV_32F);
	size_t offset = 0;
	for (int i = 0; i < ysize; i++)
		for (int j = 0; j < xsize; j++)
			amplitude.ptr<float>(i)[j] = pbuf[offset++];

	GDALClose(hDS_amp);

	/* ===================== 读取相位 ===================== */
	GDALDatasetH hDS_phase = GDALOpen(phase_file, GA_ReadOnly);
	if (hDS_phase == NULL)
	{
		fprintf(stderr, "read_slc(): failed to open %s!\n", phase_file);
		free(pbuf);
		return -1;
	}

	GDALRasterBandH hBand_phase = GDALGetRasterBand(hDS_phase, ix);
	if (hBand_phase == NULL)
	{
		fprintf(stderr, "read_slc(): failed to get phase band!\n");
		GDALClose(hDS_phase);
		free(pbuf);
		return -1;
	}

	if (GDALRasterIO(
		hBand_phase,
		GF_Read,
		0, 0,
		xsize, ysize,
		pbuf,
		xsize, ysize,
		dataType,
		0, 0) != CE_None)
	{
		fprintf(stderr, "read_slc(): RasterIO (phase) failed!\n");
		GDALClose(hDS_phase);
		free(pbuf);
		return -1;
	}

	cv::Mat phase(ysize, xsize, CV_32F);
	offset = 0;
	for (int i = 0; i < ysize; i++)
		for (int j = 0; j < xsize; j++)
			phase.ptr<float>(i)[j] = pbuf[offset++];

	GDALClose(hDS_phase);
	free(pbuf);

	/* ===================== 构造复数 SLC ===================== */
	slc.re.create(ysize, xsize, CV_32F);
	slc.im.create(ysize, xsize, CV_32F);

	for (int i = 0; i < ysize; i++)
		for (int j = 0; j < xsize; j++)
		{
			float amp = amplitude.at<float>(i, j);
			float phs = phase.at<float>(i, j);
			slc.re.ptr<float>(i)[j] = amp * cosf(phs);
			slc.im.ptr<float>(i)[j] = amp * sinf(phs);
		}

	return 0;
}

int Biomass1A_reader::read_data(
	const char* xml_file,
	const char* amp_file,
	const char* phase_file,
	const char* orbit_file)
{
	if (!xml_file || !amp_file || !phase_file || !orbit_file)
	{
		fprintf(stderr, "read_data(): input check failed!\n");
		return -1;
	}
	int ret = read_slc(amp_file, phase_file, slc);
	if (ret < 0)
	{
		fprintf(stderr, "read_data(): can't read slc from %s\n", amp_file);
		return -1;
	}
	XMLFile xmldoc, orbitdoc;
	ret = xmldoc.XMLFile_load(xml_file);
	if (ret < 0)
	{
		fprintf(stderr, "read_data(): can't load %s\n", xml_file);
		return -1;
	}
	ret = orbitdoc.XMLFile_load(orbit_file);
	if (ret < 0)
	{
		fprintf(stderr, "read_data(): can't load %s\n", orbit_file);
		return -1;
	}

	//读取轨道参数
	TiXmlElement* pnode, * pchild/*, * pchild1*/;
	int numOfstateVec = 0;
	pnode = NULL;
	ret = orbitdoc.find_node("OSV", pnode);
	while (pnode)
	{
		numOfstateVec += 1;
		pnode = pnode->NextSiblingElement();
	}

	ret = orbitdoc.find_node("OSV", pnode);
	double time, x, y, z, vx, vy, vz;
	state_vec.create(numOfstateVec, 7, CV_64F);
	for (int i = 0; i < numOfstateVec; i++)
	{
		if (!pnode) break;
		//GPS时间
		ret = orbitdoc._find_node(pnode, "UTC", pchild);
		ret = this->UTC2GPS(pchild->GetText(), &time);
		//位置x
		ret = orbitdoc._find_node(pnode, "X", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &x);
		//位置y
		ret = orbitdoc._find_node(pnode, "Y", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &y);
		//位置z
		ret = orbitdoc._find_node(pnode, "Z", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &z);
		//速度x
		ret = orbitdoc._find_node(pnode, "VX", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &vx);
		//速度y
		ret = orbitdoc._find_node(pnode, "VY", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &vy);
		//速度z
		ret = orbitdoc._find_node(pnode, "VZ", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &vz);

		//赋值
		state_vec.at<double>(i, 0) = time;
		state_vec.at<double>(i, 1) = x;
		state_vec.at<double>(i, 2) = y;
		state_vec.at<double>(i, 3) = z;
		state_vec.at<double>(i, 4) = vx;
		state_vec.at<double>(i, 5) = vy;
		state_vec.at<double>(i, 6) = vz;
		pnode = pnode->NextSiblingElement();
	}

	//拍摄起始时间
	ret = xmldoc.find_node("firstLineAzimuthTime", pnode);
	this->acquisition_start_time = pnode->GetText();
	//拍摄结束时间
	ret = xmldoc.find_node("lastLineAzimuthTime", pnode);
	this->acquisition_stop_time = pnode->GetText();
	//卫星名称
	ret = xmldoc.get_str_para("mission", this->sensor);
	//脉冲重复频率
	ret = xmldoc.get_double_para("azimuthTimeInterval", &this->prf);
	this->prf = 1.0 / this->prf;
	//中心频率
	ret = xmldoc.get_double_para("radarCarrierFrequency", &this->carrier_frequency);
	this->carrier_frequency = this->carrier_frequency;
	//最近斜距
	ret = xmldoc.get_double_para("firstSampleSlantRangeTime", &this->slant_range_first_pixel);
	this->slant_range_first_pixel = this->slant_range_first_pixel * VEL_C / 2.0;
	//距离方位采样间隔/分辨率
	ret = xmldoc.get_double_para("rangePixelSpacing", &this->range_spacing);
	ret = xmldoc.get_double_para("azimuthPixelSpacing", &this->azimuth_spacing);
	ret = xmldoc.get_double_para("rangePixelSpacing", &this->range_resolution);
	ret = xmldoc.get_double_para("azimuthPixelSpacing", &this->azimuth_resolution);

	//中心下视角incidenceAngleMidSwath
	double inc_near = 0, inc_far = 0;
	this->inc_center = (inc_near + inc_far) * 0.5;

	//四角经纬度
	ret = xmldoc.find_node("footprint", pnode);
	ret = sscanf(pnode->GetText(), "%lf %lf %lf %lf %lf %lf %lf %lf",
		&this->bottomleft_lat, &this->bottomleft_lon,
		&this->bottomright_lat, &this->bottomright_lon, 
		&this->topright_lat, &this->topright_lon, 
		&this->topleft_lat, &this->topleft_lon);

	return 0;
}

int Biomass1A_reader::write_custom_h5_data(FormatConversion& conversion, const char* dst_h5)
{
	write_common_coordinates(conversion, dst_h5);
	conversion.write_str_to_h5(dst_h5, "polarization", this->polarization.c_str());
	return 0;
}

LUTAN_reader::LUTAN_reader(const char* data_file, const char* xml_file, int mode)
{
	b_initialized = false;
	this->LT_data_file = data_file;
	this->LT_xml_file = xml_file;
	this->mode = mode;
}

LUTAN_reader::~LUTAN_reader()
{
}

int LUTAN_reader::init()
{
	if (b_initialized) return 0;
	if (LT_data_file.empty())
	{
		fprintf(stderr, "init(): input check failed!\n");
		return -1;
	}
	int ret = read_data(this->LT_xml_file.c_str(), this->LT_data_file.c_str());
	if (ret < 0)
	{
		fprintf(stderr, "init(): read_data failed!\n");
		return -1;
	}
	b_initialized = true;
	return 0;
}

int LUTAN_reader::read_slc(const char* data_file, ComplexMat& slc)
{
	if (data_file == NULL)
	{
		fprintf(stderr, "read_slc(): input check failed!\n");
		return -1;
	}

	InitializeGDALOnce();

	GDALDatasetH hDataset = GDALOpen(data_file, GA_ReadOnly);
	if (hDataset == NULL)
	{
		fprintf(stderr, "read_slc(): failed to open %s!\n", data_file);
		return -1;
	}

	int nBand = GDALGetRasterCount(hDataset);
	if (nBand < 2)
	{
		fprintf(stderr, "read_slc(): number of bands < 2!\n");
		GDALClose(hDataset);
		return -1;
	}

	/* ================= Band 1 : Real ================= */
	GDALRasterBandH hBand = GDALGetRasterBand(hDataset, 1);
	if (hBand == NULL)
	{
		fprintf(stderr, "read_slc(): failed to get band 1!\n");
		GDALClose(hDataset);
		return -1;
	}

	int xsize = GDALGetRasterBandXSize(hBand);
	int ysize = GDALGetRasterBandYSize(hBand);
	if (xsize <= 0 || ysize <= 0)
	{
		fprintf(stderr, "read_slc(): band rows and cols error!\n");
		GDALClose(hDataset);
		return -1;
	}

	short* pbuf = (short*)malloc(sizeof(short) * xsize * ysize);
	if (!pbuf)
	{
		fprintf(stderr, "read_slc(): out of memory!\n");
		GDALClose(hDataset);
		return -1;
	}

	if (GDALRasterIO(
		hBand,
		GF_Read,
		0, 0,
		xsize, ysize,
		pbuf,
		xsize, ysize,
		GDT_Int16,
		0, 0) != CE_None)
	{
		fprintf(stderr, "read_slc(): RasterIO failed on band 1!\n");
		free(pbuf);
		GDALClose(hDataset);
		return -1;
	}

	slc.re.create(ysize, xsize, CV_16S);

	size_t offset = 0;
	for (int i = 0; i < ysize; i++)
	{
		short* rowp = slc.re.ptr<short>(i);
		for (int j = 0; j < xsize; j++)
			rowp[j] = pbuf[offset++];
	}

	/* ================= Band 2 : Imag ================= */
	hBand = GDALGetRasterBand(hDataset, 2);
	if (hBand == NULL)
	{
		fprintf(stderr, "read_slc(): failed to get band 2!\n");
		free(pbuf);
		GDALClose(hDataset);
		return -1;
	}

	if (GDALRasterIO(
		hBand,
		GF_Read,
		0, 0,
		xsize, ysize,
		pbuf,
		xsize, ysize,
		GDT_Int16,
		0, 0) != CE_None)
	{
		fprintf(stderr, "read_slc(): RasterIO failed on band 2!\n");
		free(pbuf);
		GDALClose(hDataset);
		return -1;
	}

	slc.im.create(ysize, xsize, CV_16S);

	offset = 0;
	for (int i = 0; i < ysize; i++)
	{
		short* rowp = slc.im.ptr<short>(i);
		for (int j = 0; j < xsize; j++)
			rowp[j] = pbuf[offset++];
	}

	free(pbuf);
	GDALClose(hDataset);

	return 0;
}

int LUTAN_reader::read_data(const char* xml_file, const char* data_file)
{
	if (!xml_file || !data_file)
	{
		fprintf(stderr, "read_data(): input check failed!\n");
		return -1;
	}
	int ret = read_slc(data_file, slc);
	if (ret < 0)
	{
		fprintf(stderr, "read_data(): can't read slc from %s\n", data_file);
		return -1;
	}
	XMLFile xmldoc;
	ret = xmldoc.XMLFile_load(xml_file);
	if (ret < 0)
	{
		fprintf(stderr, "read_data(): can't load %s\n", xml_file);
		return -1;
	}

	//读取轨道参数
	TiXmlElement* pnode, * pchild;
	// removed unused: pchild1 (copy-paste remnant from similar XML parsing)
	int numOfstateVec;
	ret = xmldoc.get_int_para("numStateVectors", &numOfstateVec);

	ret = xmldoc.find_node("stateVec", pnode);
	double time, x, y, z, vx, vy, vz;
	state_vec.create(numOfstateVec, 7, CV_64F);
	for (int i = 0; i < numOfstateVec; i++)
	{
		if (!pnode) break;
		//GPS时间
		ret = xmldoc._find_node(pnode, "timeUTC", pchild);
		ret = UTC2GPS(pchild->GetText(), &time);
		//位置x
		ret = xmldoc._find_node(pnode, "posX", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &x);
		//位置y
		ret = xmldoc._find_node(pnode, "posY", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &y);
		//位置z
		ret = xmldoc._find_node(pnode, "posZ", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &z);
		//速度x
		ret = xmldoc._find_node(pnode, "velX", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &vx);
		//速度y
		ret = xmldoc._find_node(pnode, "velY", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &vy);
		//速度z
		ret = xmldoc._find_node(pnode, "velZ", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &vz);

		//赋值
		state_vec.at<double>(i, 0) = time;
		state_vec.at<double>(i, 1) = x;
		state_vec.at<double>(i, 2) = y;
		state_vec.at<double>(i, 3) = z;
		state_vec.at<double>(i, 4) = vx;
		state_vec.at<double>(i, 5) = vy;
		state_vec.at<double>(i, 6) = vz;
		pnode = pnode->NextSiblingElement();
	}

	//拍摄起始时间
	ret = xmldoc.find_node("start", pnode);
	ret = xmldoc._find_node(pnode, "timeUTC", pchild);
	this->acquisition_start_time = pchild->GetText();
	//拍摄结束时间
	ret = xmldoc.find_node("stop", pnode);
	ret = xmldoc._find_node(pnode, "timeUTC", pchild);
	this->acquisition_stop_time = pchild->GetText();
	//卫星名称
	ret = xmldoc.get_str_para("mission", this->sensor);
	//脉冲重复频率
	ret = xmldoc.get_double_para("PRF", &this->prf);
	//中心频率
	ret = xmldoc.get_double_para("centerFrequency", &this->carrier_frequency);
	//最近斜距
	ret = xmldoc.get_double_para("firstPixel", &this->slant_range_first_pixel);
	this->slant_range_first_pixel = this->slant_range_first_pixel * VEL_C / 2.0;
	//距离方位采样间隔/分辨率
	ret = xmldoc.get_double_para("columnSpacing", &this->range_spacing);
	ret = xmldoc.get_double_para("rowSpacing", &this->azimuth_spacing);
	ret = xmldoc.get_double_para("slantRangeResolution", &this->range_resolution);
	ret = xmldoc.get_double_para("azimuthResolution", &this->azimuth_resolution);

	//中心下视角incidenceAngleMidSwath
	ret = xmldoc.get_double_para("incidenceAngle", &this->inc_center);

	//四角经纬度
	ret = xmldoc.find_node("sceneCornerCoord", pnode);
	//bottomleft
	ret = xmldoc._find_node(pnode, "lat", pchild);
	ret = sscanf(pchild->GetText(), "%lf", &this->bottomleft_lat);
	ret = xmldoc._find_node(pnode, "lon", pchild);
	ret = sscanf(pchild->GetText(), "%lf", &this->bottomleft_lon);
	//bottomRight
	pnode = pnode->NextSiblingElement();
	ret = xmldoc._find_node(pnode, "lat", pchild);
	ret = sscanf(pchild->GetText(), "%lf", &this->bottomright_lat);
	ret = xmldoc._find_node(pnode, "lon", pchild);
	ret = sscanf(pchild->GetText(), "%lf", &this->bottomright_lon);
	//topLeft
	pnode = pnode->NextSiblingElement();
	ret = xmldoc._find_node(pnode, "lat", pchild);
	ret = sscanf(pchild->GetText(), "%lf", &this->topleft_lat);
	ret = xmldoc._find_node(pnode, "lon", pchild);
	ret = sscanf(pchild->GetText(), "%lf", &this->topleft_lon);
	//topRight
	pnode = pnode->NextSiblingElement();
	ret = xmldoc._find_node(pnode, "lat", pchild);
	ret = sscanf(pchild->GetText(), "%lf", &this->topright_lat);
	ret = xmldoc._find_node(pnode, "lon", pchild);
	ret = sscanf(pchild->GetText(), "%lf", &this->topright_lon);

	return 0;
}

int LUTAN_reader::write_custom_h5_data(FormatConversion& conversion, const char* dst_h5)
{
	write_common_coordinates(conversion, dst_h5);
	conversion.write_int_to_h5(dst_h5, "TR_mode", this->mode);
	return 0;
}




Spacety_reader::Spacety_reader(const char* data_file, const char* xml_file)
{
	b_initialized = false;
	this->Spacety_data_file = data_file;
	this->Spacety_xml_file = xml_file;
}

Spacety_reader::~Spacety_reader()
{
}

int Spacety_reader::init()
{
	if (b_initialized) return 0;
	if (Spacety_data_file.empty())
	{
		fprintf(stderr, "init(): input check failed!\n");
		return -1;
	}
	int ret = read_data(this->Spacety_xml_file.c_str(), this->Spacety_data_file.c_str());
	if (ret < 0)
	{
		fprintf(stderr, "init(): read_data failed!\n");
		return -1;
	}
	b_initialized = true;
	return 0;
}

int Spacety_reader::init_test()
{
	if (b_initialized) return 0;
	if (Spacety_data_file.empty())
	{
		fprintf(stderr, "init(): input check failed!\n");
		return -1;
	}
	int ret = read_data_test(this->Spacety_xml_file.c_str(), this->Spacety_data_file.c_str());
	if (ret < 0)
	{
		fprintf(stderr, "init(): read_data failed!\n");
		return -1;
	}
	b_initialized = true;
	return 0;
}

//int Spacety_reader::read_slc(const char* data_file, ComplexMat& slc)
//{
//	if (data_file == NULL)
//	{
//		fprintf(stderr, "read_slc(): input check failed!\n");
//		return -1;
//	}
//
//	GDALAllRegister();
//
//	GDALDatasetH hDataset = GDALOpen(data_file, GA_ReadOnly);
//	if (hDataset == NULL)
//	{
//		fprintf(stderr, "read_slc(): failed to open %s!\n", data_file);
//		return -1;
//	}
//
//	int nBand = GDALGetRasterCount(hDataset);
//
//	/* =========================================================
//	 * 情况一：单波段 packed complex（Int32）
//	 * 低 16 bit : Real
//	 * 高 16 bit : Imag
//	 * ========================================================= */
//	if (nBand == 1)
//	{
//		GDALRasterBandH hBand = GDALGetRasterBand(hDataset, 1);
//		if (hBand == NULL)
//		{
//			fprintf(stderr, "read_slc(): failed to get band 1!\n");
//			GDALClose(hDataset);
//			return -1;
//		}
//
//		int xsize = GDALGetRasterBandXSize(hBand);
//		int ysize = GDALGetRasterBandYSize(hBand);
//		if (xsize <= 0 || ysize <= 0)
//		{
//			fprintf(stderr, "read_slc(): band rows and cols error!\n");
//			GDALClose(hDataset);
//			return -1;
//		}
//
//		int* pbuf = (int*)malloc(sizeof(int) * xsize * ysize);
//		if (!pbuf)
//		{
//			fprintf(stderr, "read_slc(): out of memory!\n");
//			GDALClose(hDataset);
//			return -1;
//		}
//
//		if (GDALRasterIO(
//			hBand,
//			GF_Read,
//			0, 0,
//			xsize, ysize,
//			pbuf,
//			xsize, ysize,
//			GDT_Int32,
//			0, 0) != CE_None)
//		{
//			fprintf(stderr, "read_slc(): RasterIO failed!\n");
//			free(pbuf);
//			GDALClose(hDataset);
//			return -1;
//		}
//
//		slc.re.create(ysize, xsize, CV_16S);
//		slc.im.create(ysize, xsize, CV_16S);
//
//		for (int i = 0; i < ysize; i++)
//		{
//			short* re_row = slc.re.ptr<short>(i);
//			short* im_row = slc.im.ptr<short>(i);
//			for (int j = 0; j < xsize; j++)
//			{
//				int v = pbuf[j + i * xsize];
//				re_row[j] = (short)(v & 0xFFFF);
//				im_row[j] = (short)((v >> 16) & 0xFFFF);
//			}
//		}
//
//		free(pbuf);
//		GDALClose(hDataset);
//	}
//	/* =========================================================
//	 * 情况二：双波段 SLC
//	 * Band 1 : Real (Int16)
//	 * Band 2 : Imag (Int16)
//	 * ========================================================= */
//	else
//	{
//		GDALRasterBandH hBand = GDALGetRasterBand(hDataset, 1);
//		if (hBand == NULL)
//		{
//			fprintf(stderr, "read_slc(): failed to get band 1!\n");
//			GDALClose(hDataset);
//			return -1;
//		}
//
//		int xsize = GDALGetRasterBandXSize(hBand);
//		int ysize = GDALGetRasterBandYSize(hBand);
//		if (xsize <= 0 || ysize <= 0)
//		{
//			fprintf(stderr, "read_slc(): band rows and cols error!\n");
//			GDALClose(hDataset);
//			return -1;
//		}
//
//		short* pbuf = (short*)malloc(sizeof(short) * xsize * ysize);
//		if (!pbuf)
//		{
//			fprintf(stderr, "read_slc(): out of memory!\n");
//			GDALClose(hDataset);
//			return -1;
//		}
//
//		/* ---------- Band 1 : Real ---------- */
//		if (GDALRasterIO(
//			hBand,
//			GF_Read,
//			0, 0,
//			xsize, ysize,
//			pbuf,
//			xsize, ysize,
//			GDT_Int16,
//			0, 0) != CE_None)
//		{
//			fprintf(stderr, "read_slc(): RasterIO failed on band 1!\n");
//			free(pbuf);
//			GDALClose(hDataset);
//			return -1;
//		}
//
//		slc.re.create(ysize, xsize, CV_16S);
//
//		size_t offset = 0;
//		for (int i = 0; i < ysize; i++)
//		{
//			short* row = slc.re.ptr<short>(i);
//			for (int j = 0; j < xsize; j++)
//				row[j] = pbuf[offset++];
//		}
//
//		/* ---------- Band 2 : Imag ---------- */
//		hBand = GDALGetRasterBand(hDataset, 2);
//		if (hBand == NULL)
//		{
//			fprintf(stderr, "read_slc(): failed to get band 2!\n");
//			free(pbuf);
//			GDALClose(hDataset);
//			return -1;
//		}
//
//		if (GDALRasterIO(
//			hBand,
//			GF_Read,
//			0, 0,
//			xsize, ysize,
//			pbuf,
//			xsize, ysize,
//			GDT_Int16,
//			0, 0) != CE_None)
//		{
//			fprintf(stderr, "read_slc(): RasterIO failed on band 2!\n");
//			free(pbuf);
//			GDALClose(hDataset);
//			return -1;
//		}
//
//		slc.im.create(ysize, xsize, CV_16S);
//
//		offset = 0;
//		for (int i = 0; i < ysize; i++)
//		{
//			short* row = slc.im.ptr<short>(i);
//			for (int j = 0; j < xsize; j++)
//				row[j] = pbuf[offset++];
//		}
//
//		free(pbuf);
//		GDALClose(hDataset);
//	}
//
//	return 0;
//}

int Spacety_reader::read_slc(const char* data_file, ComplexMat& slc)
{
	if (data_file == NULL)
	{
		fprintf(stderr, "read_slc(): input check failed!\n");
		return -1;
	}

	InitializeGDALOnce();

	GDALDatasetH hDataset = GDALOpen(data_file, GA_ReadOnly);
	if (hDataset == NULL)
	{
		fprintf(stderr, "read_slc(): failed to open %s!\n", data_file);
		return -1;
	}

	int nBand = GDALGetRasterCount(hDataset);
	if (nBand < 1)
	{
		fprintf(stderr, "read_slc(): no raster band found!\n");
		GDALClose(hDataset);
		return -1;
	}

	GDALRasterBandH hBand1 = GDALGetRasterBand(hDataset, 1);
	if (hBand1 == NULL)
	{
		fprintf(stderr, "read_slc(): failed to get band 1!\n");
		GDALClose(hDataset);
		return -1;
	}

	int xsize = GDALGetRasterBandXSize(hBand1);
	int ysize = GDALGetRasterBandYSize(hBand1);
	if (xsize <= 0 || ysize <= 0)
	{
		fprintf(stderr, "read_slc(): band rows and cols error!\n");
		GDALClose(hDataset);
		return -1;
	}

	GDALDataType dt = GDALGetRasterDataType(hBand1);
	fprintf(stderr, "Band1 type = %s\n", GDALGetDataTypeName(dt));

	/* =========================================================
	 * 情况一：单波段 GDAL 复数类型，例如 CInt16
	 * ========================================================= */
	if (nBand == 1 && dt == GDT_CInt16)
	{
		short* pbuf = (short*)malloc(sizeof(short) * xsize * ysize * 2);
		if (!pbuf)
		{
			fprintf(stderr, "read_slc(): out of memory!\n");
			GDALClose(hDataset);
			return -1;
		}

		if (GDALRasterIO(
			hBand1,
			GF_Read,
			0, 0,
			xsize, ysize,
			pbuf,
			xsize, ysize,
			GDT_CInt16,
			0, 0) != CE_None)
		{
			fprintf(stderr, "read_slc(): RasterIO failed on complex band!\n");
			free(pbuf);
			GDALClose(hDataset);
			return -1;
		}

		slc.re.create(ysize, xsize, CV_16S);
		slc.im.create(ysize, xsize, CV_16S);

		size_t idx = 0;
		for (int i = 0; i < ysize; i++)
		{
			short* re_row = slc.re.ptr<short>(i);
			short* im_row = slc.im.ptr<short>(i);
			for (int j = 0; j < xsize; j++)
			{
				re_row[j] = pbuf[idx++];   // real
				im_row[j] = pbuf[idx++];   // imag
			}
		}

		free(pbuf);
		GDALClose(hDataset);
		return 0;
	}

	/* =========================================================
	 * 情况二：单波段 packed complex（自定义 Int32）
	 * 低 16 bit : Real
	 * 高 16 bit : Imag
	 * ========================================================= */
	if (nBand == 1 && dt == GDT_Int32)
	{
		int* pbuf = (int*)malloc(sizeof(int) * xsize * ysize);
		if (!pbuf)
		{
			fprintf(stderr, "read_slc(): out of memory!\n");
			GDALClose(hDataset);
			return -1;
		}

		if (GDALRasterIO(
			hBand1,
			GF_Read,
			0, 0,
			xsize, ysize,
			pbuf,
			xsize, ysize,
			GDT_Int32,
			0, 0) != CE_None)
		{
			fprintf(stderr, "read_slc(): RasterIO failed!\n");
			free(pbuf);
			GDALClose(hDataset);
			return -1;
		}

		slc.re.create(ysize, xsize, CV_16S);
		slc.im.create(ysize, xsize, CV_16S);

		for (int i = 0; i < ysize; i++)
		{
			short* re_row = slc.re.ptr<short>(i);
			short* im_row = slc.im.ptr<short>(i);
			for (int j = 0; j < xsize; j++)
			{
				unsigned int v = (unsigned int)pbuf[j + i * xsize];
				re_row[j] = (short)(v & 0xFFFF);
				im_row[j] = (short)((v >> 16) & 0xFFFF);
			}
		}

		free(pbuf);
		GDALClose(hDataset);
		return 0;
	}

	/* =========================================================
	 * 情况三：双波段 SLC
	 * Band 1 : Real
	 * Band 2 : Imag
	 * ========================================================= */
	if (nBand >= 2)
	{
		GDALRasterBandH hBand2 = GDALGetRasterBand(hDataset, 2);
		if (hBand2 == NULL)
		{
			fprintf(stderr, "read_slc(): failed to get band 2!\n");
			GDALClose(hDataset);
			return -1;
		}

		short* pbuf = (short*)malloc(sizeof(short) * xsize * ysize);
		if (!pbuf)
		{
			fprintf(stderr, "read_slc(): out of memory!\n");
			GDALClose(hDataset);
			return -1;
		}

		if (GDALRasterIO(
			hBand1,
			GF_Read,
			0, 0,
			xsize, ysize,
			pbuf,
			xsize, ysize,
			GDT_Int16,
			0, 0) != CE_None)
		{
			fprintf(stderr, "read_slc(): RasterIO failed on band 1!\n");
			free(pbuf);
			GDALClose(hDataset);
			return -1;
		}

		slc.re.create(ysize, xsize, CV_16S);
		for (int i = 0; i < ysize; i++)
		{
			short* row = slc.re.ptr<short>(i);
			memcpy(row, pbuf + (size_t)i * xsize, sizeof(short) * xsize);
		}

		if (GDALRasterIO(
			hBand2,
			GF_Read,
			0, 0,
			xsize, ysize,
			pbuf,
			xsize, ysize,
			GDT_Int16,
			0, 0) != CE_None)
		{
			fprintf(stderr, "read_slc(): RasterIO failed on band 2!\n");
			free(pbuf);
			GDALClose(hDataset);
			return -1;
		}

		slc.im.create(ysize, xsize, CV_16S);
		for (int i = 0; i < ysize; i++)
		{
			short* row = slc.im.ptr<short>(i);
			memcpy(row, pbuf + (size_t)i * xsize, sizeof(short) * xsize);
		}

		free(pbuf);
		GDALClose(hDataset);
		return 0;
	}

	fprintf(stderr, "read_slc(): unsupported data format!\n");
	GDALClose(hDataset);
	return -1;
}

int Spacety_reader::read_data(const char* xml_file, const char* data_file)
{
	if (!xml_file || !data_file)
	{
		fprintf(stderr, "read_data(): input check failed!\n");
		return -1;
	}
	int ret = read_slc(data_file, slc);
	if (ret < 0)
	{
		fprintf(stderr, "read_data(): can't read slc from %s\n", data_file);
		return -1;
	}
	XMLFile xmldoc;
	ret = xmldoc.XMLFile_load(xml_file);
	if (ret < 0)
	{
		fprintf(stderr, "read_data(): can't load %s\n", xml_file);
		return -1;
	}

	//读取轨道参数
	TiXmlElement* pnode, * pchild, * pchild1;
	int numOfstateVec;
	ret = xmldoc.find_node("orbitList", pnode);
	ret = sscanf(pnode->FirstAttribute()->Value(), "%d", &numOfstateVec);

	ret = xmldoc.find_node("orbit", pnode);
	double time, x, y, z, vx, vy, vz;
	state_vec.create(numOfstateVec, 7, CV_64F);
	for (int i = 0; i < numOfstateVec; i++)
	{
		if (!pnode) break;
		//GPS时间
		ret = xmldoc._find_node(pnode, "time", pchild);
		ret = UTC2GPS(pchild->GetText(), &time);
		ret = xmldoc._find_node(pnode, "position", pchild1);
		//位置x
		ret = xmldoc._find_node(pchild1, "x", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &x);
		//位置y
		ret = xmldoc._find_node(pchild1, "y", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &y);
		//位置z
		ret = xmldoc._find_node(pchild1, "z", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &z);

		ret = xmldoc._find_node(pnode, "velocity", pchild1);
		//速度x
		ret = xmldoc._find_node(pchild1, "x", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &vx);
		//速度y
		ret = xmldoc._find_node(pchild1, "y", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &vy);
		//速度z
		ret = xmldoc._find_node(pchild1, "z", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &vz);

		//赋值
		state_vec.at<double>(i, 0) = time;
		state_vec.at<double>(i, 1) = x;
		state_vec.at<double>(i, 2) = y;
		state_vec.at<double>(i, 3) = z;
		state_vec.at<double>(i, 4) = vx;
		state_vec.at<double>(i, 5) = vy;
		state_vec.at<double>(i, 6) = vz;
		pnode = pnode->NextSiblingElement();
	}

	//拍摄起始时间
	ret = xmldoc.get_str_para("productFirstLineUtcTime", this->acquisition_start_time);
	//拍摄结束时间
	ret = xmldoc.get_str_para("productLastLineUtcTime", this->acquisition_stop_time);
	//卫星名称
	this->sensor = "fucheng-1";
	//脉冲重复频率
	ret = xmldoc.get_double_para("azimuthTimeInterval", &this->prf);
	this->prf = 1.0 / this->prf;
	//中心频率
	ret = xmldoc.get_double_para("radarFrequency", &this->carrier_frequency);
	//this->carrier_frequency = this->carrier_frequency * 1e9;
	//最近斜距
	ret = xmldoc.get_double_para("slantRangeTime", &this->slant_range_first_pixel);
	this->slant_range_first_pixel = this->slant_range_first_pixel * VEL_C / 2.0;
	//距离方位采样间隔/分辨率
	ret = xmldoc.get_double_para("rangePixelSpacing", &this->range_spacing);
	ret = xmldoc.get_double_para("azimuthPixelSpacing", &this->azimuth_spacing);
	//ret = xmldoc.get_double_para("rangeResolution", &this->range_resolution);
	//ret = xmldoc.get_double_para("azimuthResolution", &this->azimuth_resolution);
	
	//中心下视角incidenceAngleMidSwath
	ret = xmldoc.get_double_para("incidenceAngleMidSwath", &this->inc_center);

	//四角经纬度
	int num_geolocation_points = 0;
	ret = xmldoc.find_node("geolocationGridPointList", pnode);
	ret = sscanf(pnode->FirstAttribute()->Value(), "%d", &num_geolocation_points);
	ret = xmldoc.find_node("geolocationGridPoint", pnode);
	int line, pixel;
	// removed unused: latitude, longitude (geolocation grid parsing not completed)
	for (int i = 0; i < num_geolocation_points; i++)
	{
		if (!pnode) break;
		//行数
		ret = xmldoc._find_node(pnode, "line", pchild);
		ret = sscanf(pchild->GetText(), "%d", &line);
		//列数
		ret = xmldoc._find_node(pnode, "pixel", pchild);
		ret = sscanf(pchild->GetText(), "%d", &pixel);
		if (line == 0 && pixel == 0)
		{
			ret = xmldoc._find_node(pnode, "latitude", pchild);
			ret = sscanf(pchild->GetText(), "%lf", &this->topleft_lat);
			ret = xmldoc._find_node(pnode, "longitude", pchild);
			ret = sscanf(pchild->GetText(), "%lf", &this->topleft_lon);
		}
		if (line == 0 && pixel != 0)
		{
			ret = xmldoc._find_node(pnode, "latitude", pchild);
			ret = sscanf(pchild->GetText(), "%lf", &this->topright_lat);
			ret = xmldoc._find_node(pnode, "longitude", pchild);
			ret = sscanf(pchild->GetText(), "%lf", &this->topright_lon);
		}
		if (line != 0 && pixel == 0)
		{
			ret = xmldoc._find_node(pnode, "latitude", pchild);
			ret = sscanf(pchild->GetText(), "%lf", &this->bottomleft_lat);
			ret = xmldoc._find_node(pnode, "longitude", pchild);
			ret = sscanf(pchild->GetText(), "%lf", &this->bottomleft_lon);
		}
		if (line != 0 && pixel != 0)
		{
			ret = xmldoc._find_node(pnode, "latitude", pchild);
			ret = sscanf(pchild->GetText(), "%lf", &this->bottomright_lat);
			ret = xmldoc._find_node(pnode, "longitude", pchild);
			ret = sscanf(pchild->GetText(), "%lf", &this->bottomright_lon);
		}
		pnode = pnode->NextSiblingElement();
	}

	return 0;
}

int Spacety_reader::read_data_test(const char* xml_file, const char* data_file)
{
	if (!xml_file || !data_file)
	{
		fprintf(stderr, "read_data(): input check failed!\n");
		return -1;
	}
	int ret = read_slc(data_file, slc);
	if (ret < 0)
	{
		fprintf(stderr, "read_data(): can't read slc from %s\n", data_file);
		return -1;
	}
	XMLFile xmldoc;
	ret = xmldoc.XMLFile_load(xml_file);
	if (ret < 0)
	{
		fprintf(stderr, "read_data(): can't load %s\n", xml_file);
		return -1;
	}

	//读取轨道参数
	TiXmlElement* pnode, * pchild;
	// removed unused: pchild1 (copy-paste remnant from similar XML parsing)
	int numOfstateVec = 6;
	pnode = NULL;
	double time, x, y, z, vx, vy, vz;
	state_vec.create(numOfstateVec, 7, CV_64F);
	for (int i = 0; i < numOfstateVec; i++)
	{
		if (i == 0) ret = xmldoc.find_node("GPSParam", pnode);
		
		//GPS时间
		ret = xmldoc._find_node(pnode, "TimeStamp", pchild);
		string tmp = pchild->GetText();
		int rpos = static_cast<int>(tmp.rfind("-"));
		string tmp2 = tmp.substr(0, rpos) + "T" + tmp.substr(rpos + 1, tmp.length() - rpos - 1);
		ret = UTC2GPS(tmp2.c_str(), &time);
		//位置x
		ret = xmldoc._find_node(pnode, "xPosition", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &x);
		//位置y
		ret = xmldoc._find_node(pnode, "yPosition", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &y);
		//位置z
		ret = xmldoc._find_node(pnode, "zPosition", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &z);

		//速度x
		ret = xmldoc._find_node(pnode, "xVelocity", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &vx);
		//速度y
		ret = xmldoc._find_node(pnode, "yVelocity", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &vy);
		//速度z
		ret = xmldoc._find_node(pnode, "zVelocity", pchild);
		ret = sscanf(pchild->GetText(), "%lf", &vz);

		//赋值
		state_vec.at<double>(i, 0) = time;
		state_vec.at<double>(i, 1) = x;
		state_vec.at<double>(i, 2) = y;
		state_vec.at<double>(i, 3) = z;
		state_vec.at<double>(i, 4) = vx;
		state_vec.at<double>(i, 5) = vy;
		state_vec.at<double>(i, 6) = vz;
		pnode = pnode->NextSiblingElement();
	}

	//拍摄起始时间
	ret = xmldoc.get_str_para("imagingTimestart", this->acquisition_start_time);
	string tmp = this->acquisition_start_time;
	int rpos = static_cast<int>(tmp.rfind("-"));
	this->acquisition_start_time = tmp.substr(0, rpos) + "T" + tmp.substr(rpos + 1, tmp.length() - rpos - 1);
	//拍摄结束时间
	ret = xmldoc.get_str_para("imagingTimeend", this->acquisition_stop_time);
	tmp = this->acquisition_stop_time;
	rpos = static_cast<int>(tmp.rfind("-"));
	this->acquisition_stop_time = tmp.substr(0, rpos) + "T" + tmp.substr(rpos + 1, tmp.length() - rpos - 1);
	//卫星名称
	this->sensor = "fucheng-1";
	//脉冲重复频率
	ret = xmldoc.get_double_para("eqvPRF", &this->prf);
	//中心频率
	ret = xmldoc.get_double_para("RadarCenterFrequency", &this->carrier_frequency);
	this->carrier_frequency = this->carrier_frequency * 1e9;
	//最近斜距
	ret = xmldoc.get_double_para("nearRange", &this->slant_range_first_pixel);
	//this->slant_range_first_pixel = this->slant_range_first_pixel * VEL_C / 2.0;
	//距离方位采样间隔/分辨率
	ret = xmldoc.get_double_para("widthspace", &this->range_spacing);
	ret = xmldoc.get_double_para("heightspace", &this->azimuth_spacing);
	//ret = xmldoc.get_double_para("rangeResolution", &this->range_resolution);
	//ret = xmldoc.get_double_para("azimuthResolution", &this->azimuth_resolution);

	//中心下视角incidenceAngleMidSwath
	ret = xmldoc.get_double_para("incidenceAngleNearRange", &this->inc_center);

	//四角经纬度
	double latitude, longitude;
	ret = xmldoc.get_double_para("latitude", &latitude);
	ret = xmldoc.get_double_para("longitude", &longitude);
	this->topleft_lat = latitude + 0.2;
	this->bottomleft_lat = latitude - 0.2;
	this->topright_lat = latitude + 0.2;
	this->bottomright_lat = latitude - 0.2;
	this->topleft_lon = longitude - 0.2;
	this->topright_lon = longitude + 0.2;
	this->bottomleft_lon = longitude - 0.2;
	this->bottomright_lon = longitude + 0.2;

	return 0;
}

int Spacety_reader::write_custom_h5_data(FormatConversion& conversion, const char* dst_h5)
{
	write_common_coordinates(conversion, dst_h5);
	return 0;
}
