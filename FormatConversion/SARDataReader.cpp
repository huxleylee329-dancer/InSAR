#include "pch.h"
#include "..\include\FormatConversion.h"
#include "..\include\Hdf5IO.h"

#include <cstdio>

namespace
{
	class Hdf5BatchGuard
	{
	public:
		Hdf5BatchGuard() : lock_(Hdf5IO::acquireBatchLock()) {}
		~Hdf5BatchGuard() { Hdf5IO::releaseBatchLock(lock_); }
		int status() const { return Hdf5IO::getBatchLockStatus(lock_); }

	private:
		Hdf5IO::BatchLock* lock_;
	};
}
SARDataReader::SARDataReader()
	: b_initialized(false)
	, azimuth_resolution(0.0)
	, azimuth_spacing(0.0)
	, carrier_frequency(0.0)
	, prf(0.0)
	, range_resolution(0.0)
	, range_spacing(0.0)
	, slant_range_first_pixel(0.0)
	, slant_range_last_pixel(0.0)
	, inc_center(0.0)
	, topleft_lon(0.0), topright_lon(0.0), bottomleft_lon(0.0), bottomright_lon(0.0)
	, topleft_lat(0.0), topright_lat(0.0), bottomleft_lat(0.0), bottomright_lat(0.0)
{
}

SARDataReader::~SARDataReader()
{
}

int SARDataReader::write_to_h5(const char* dst_h5)
{
	Hdf5BatchGuard hdf5Batch;
	if (hdf5Batch.status() != 0) return hdf5Batch.status();
	if (!dst_h5)
	{
		fprintf(stderr, "write_to_h5(): input check failed!\n");
		return -1;
	}
	int ret;
	if (!b_initialized)
	{
		ret = init();
		if (ret < 0)
		{
			fprintf(stderr, "write_to_h5(): init() failed!\n");
			return -1;
		}
	}
	FormatConversion conversion;
	ret = conversion.creat_new_h5(dst_h5);
	if (ret < 0)
	{
		fprintf(stderr, "write_to_h5(): failed to create %s!\n", dst_h5);
		return -1;
	}

	conversion.write_array_to_h5(dst_h5, "state_vec", this->state_vec);

	conversion.write_double_to_h5(dst_h5, "azimuth_spacing", this->azimuth_spacing);
	conversion.write_double_to_h5(dst_h5, "range_spacing", this->range_spacing);
	conversion.write_double_to_h5(dst_h5, "slant_range_first_pixel", this->slant_range_first_pixel);
	conversion.write_double_to_h5(dst_h5, "carrier_frequency", this->carrier_frequency);
	conversion.write_double_to_h5(dst_h5, "prf", this->prf);

	conversion.write_str_to_h5(dst_h5, "sensor", this->sensor.c_str());
	conversion.write_str_to_h5(dst_h5, "acquisition_start_time", this->acquisition_start_time.c_str());
	conversion.write_str_to_h5(dst_h5, "acquisition_stop_time", this->acquisition_stop_time.c_str());

	conversion.write_int_to_h5(dst_h5, "azimuth_len", slc.GetRows());
	conversion.write_int_to_h5(dst_h5, "range_len", slc.GetCols());

	ret = write_custom_h5_data(conversion, dst_h5);
	if (ret < 0)
	{
		return -1;
	}

	conversion.write_slc_to_h5(dst_h5, slc);

	return 0;
}

int SARDataReader::write_common_coordinates(FormatConversion& conversion, const char* dst_h5)
{
	conversion.write_double_to_h5(dst_h5, "inc_center", this->inc_center);
	conversion.write_double_to_h5(dst_h5, "topLeftLat", this->topleft_lat);
	conversion.write_double_to_h5(dst_h5, "topLeftLon", this->topleft_lon);
	conversion.write_double_to_h5(dst_h5, "topRightLat", this->topright_lat);
	conversion.write_double_to_h5(dst_h5, "topRightLon", this->topright_lon);
	conversion.write_double_to_h5(dst_h5, "bottomLeftLat", this->bottomleft_lat);
	conversion.write_double_to_h5(dst_h5, "bottomLeftLon", this->bottomleft_lon);
	conversion.write_double_to_h5(dst_h5, "bottomRightLat", this->bottomright_lat);
	conversion.write_double_to_h5(dst_h5, "bottomRightLon", this->bottomright_lon);

	conversion.write_int_to_h5(dst_h5, "offset_row", 0);
	conversion.write_int_to_h5(dst_h5, "offset_col", 0);

	return 0;
}


