#include "stdafx.h"
#include "../include/Utils.h"

#include <algorithm>
#include <cmath>

using namespace cv;

// orbitStateVectors implementations
// ====================================================

namespace
{
bool hasValidOrbitStateVectors(const Mat& vectors, int minimumRows)
{
    if (vectors.type() != CV_64F || vectors.cols != 7 || vectors.rows < minimumRows)
    {
        return false;
    }

    double previousTime = 0.0;
    for (int row = 0; row < vectors.rows; ++row)
    {
        const double time = vectors.at<double>(row, 0);
        if (!std::isfinite(time) || (row > 0 && !(time > previousTime)))
        {
            return false;
        }
        for (int column = 1; column < vectors.cols; ++column)
        {
            if (!std::isfinite(vectors.at<double>(row, column)))
            {
                return false;
            }
        }
        previousTime = time;
    }
    return true;
}

bool evaluateCubicHermite(const Mat& vectors, double time, Position* position, Velocity* velocity)
{
    if ((!position && !velocity) || !std::isfinite(time) || !hasValidOrbitStateVectors(vectors, 2) ||
        time < vectors.at<double>(0, 0) || time > vectors.at<double>(vectors.rows - 1, 0))
    {
        return false;
    }

    int lower = 0;
    int upper = vectors.rows - 1;
    if (time < vectors.at<double>(upper, 0))
    {
        while (upper - lower > 1)
        {
            const int middle = lower + (upper - lower) / 2;
            if (vectors.at<double>(middle, 0) <= time)
            {
                lower = middle;
            }
            else
            {
                upper = middle;
            }
        }
    }
    else
    {
        lower = upper - 1;
    }

    const double t0 = vectors.at<double>(lower, 0);
    const double interval = vectors.at<double>(upper, 0) - t0;
    if (!std::isfinite(interval) || interval <= 0.0)
    {
        return false;
    }
    const double s = (time - t0) / interval;
    const double s2 = s * s;
    const double s3 = s2 * s;
    const double h00 = 2.0 * s3 - 3.0 * s2 + 1.0;
    const double h10 = s3 - 2.0 * s2 + s;
    const double h01 = -2.0 * s3 + 3.0 * s2;
    const double h11 = s3 - s2;
    const double dh00 = 6.0 * s2 - 6.0 * s;
    const double dh10 = 3.0 * s2 - 4.0 * s + 1.0;
    const double dh01 = -6.0 * s2 + 6.0 * s;
    const double dh11 = 3.0 * s2 - 2.0 * s;

    double values[3] = {};
    double derivatives[3] = {};
    for (int component = 0; component < 3; ++component)
    {
        const double p0 = vectors.at<double>(lower, component + 1);
        const double p1 = vectors.at<double>(upper, component + 1);
        const double v0 = vectors.at<double>(lower, component + 4);
        const double v1 = vectors.at<double>(upper, component + 4);
        values[component] = h00 * p0 + h10 * interval * v0 + h01 * p1 + h11 * interval * v1;
        derivatives[component] = (dh00 * p0 + dh10 * interval * v0 + dh01 * p1 + dh11 * interval * v1) / interval;
        if (!std::isfinite(values[component]) || !std::isfinite(derivatives[component]))
        {
            return false;
        }
    }

    if (position)
    {
        position->x = values[0];
        position->y = values[1];
        position->z = values[2];
    }
    if (velocity)
    {
        velocity->vx = derivatives[0];
        velocity->vy = derivatives[1];
        velocity->vz = derivatives[2];
    }
    return true;
}
}

orbitStateVectors::orbitStateVectors(const Mat& stateVectors, double startTime, double stopTime)
{
	this->startTime = startTime;
	this->stopTime = stopTime;
	this->isOrbitUpdated = false;
	this->interpolationMode = OrbitInterpolationMode::ResampledLagrange;
	stateVectors.copyTo(this->stateVectors);
	if (this->stateVectors.type() != CV_64F) this->stateVectors.convertTo(this->stateVectors, CV_64F);
	double delta_time = 0.0;
	if (hasValidOrbitStateVectors(this->stateVectors, 2))
	{
		delta_time = this->stateVectors.at<double>(1, 0) - this->stateVectors.at<double>(0, 0);
		if (delta_time <= 1.0)
		{
			this->isOrbitUpdated = true;
			this->stateVectors.copyTo(this->newStateVectors);
		}
	}
	this->dt = delta_time;
	setSceneStartStopTime(startTime, stopTime);

}

orbitStateVectors::orbitStateVectors(const Mat& stateVectors, double startTime, double stopTime, double delta_time)
{
	this->dt = delta_time;
	this->isOrbitUpdated = false;
	this->interpolationMode = OrbitInterpolationMode::ResampledLagrange;
	stateVectors.copyTo(this->stateVectors);
	if (this->stateVectors.type() != CV_64F) this->stateVectors.convertTo(this->stateVectors, CV_64F);
	if (hasValidOrbitStateVectors(this->stateVectors, 2) && std::isfinite(delta_time) &&
		delta_time > 0.0 && delta_time <= 1.0)
	{
		this->isOrbitUpdated = true;
		this->stateVectors.copyTo(this->newStateVectors);
	}
	setSceneStartStopTime(startTime, stopTime);
}

orbitStateVectors::orbitStateVectors(const Mat& stateVectors, double startTime, double stopTime,
	OrbitInterpolationMode interpolationMode)
	: orbitStateVectors(stateVectors, startTime, stopTime)
{
	this->interpolationMode = interpolationMode;
}

orbitStateVectors::~orbitStateVectors()
{
}

int orbitStateVectors::setSceneStartStopTime(double startTime, double stopTime)
{
	this->startTime = startTime;
	this->stopTime = stopTime;
	return 0;
}

double orbitStateVectors::get_start_time()
{
	return startTime;
}

double orbitStateVectors::get_stop_time()
{
	return stopTime;
}

int orbitStateVectors::getPosition(double azimuthTime, Position& position)
{
	if (interpolationMode == OrbitInterpolationMode::FineV2CubicHermite)
	{
		return evaluateCubicHermite(stateVectors, azimuthTime, &position, nullptr) ? 0 : -1;
	}
	if (newStateVectors.cols != 7 || newStateVectors.rows < 2 || !isOrbitUpdated)
	{
		fprintf(stderr, "getPosition(): input check failed!\n");
		return -1;
	}
	if (azimuthTime < newStateVectors.at<double>(0, 0) || azimuthTime > newStateVectors.at<double>(newStateVectors.rows - 1, 0))
	{
		fprintf(stderr, "getPosition(): azimuthTime out of legal range\n");
		return -1;
	}
	int i0, iN;
	if (newStateVectors.rows <= nv) {
		i0 = 0;
		iN = newStateVectors.rows - 1;
	}
	else {
		i0 = std::max((int)((azimuthTime - newStateVectors.at<double>(0, 0)) / dt) - nv / 2 + 1, 0);
		iN = std::min(i0 + nv - 1, newStateVectors.rows - 1);
		i0 = (iN < newStateVectors.rows - 1 ? i0 : iN - nv + 1);
	}
	position.x = 0.0;
	position.y = 0.0;
	position.z = 0.0;
	for (int i = i0; i <= iN; ++i) {
		double weight = 1;
		for (int j = i0; j <= iN; ++j) {
			if (j != i) {
				double time2 = newStateVectors.at<double>(j, 0);
				weight *= (azimuthTime - time2) / (newStateVectors.at<double>(i, 0) - time2);
			}
		}
		position.x += weight * newStateVectors.at<double>(i, 1);
		position.y += weight * newStateVectors.at<double>(i, 2);
		position.z += weight * newStateVectors.at<double>(i, 3);
	}
	return 0;
}

int orbitStateVectors::getVelocity(double azimuthTime, Velocity& velocity)
{
	if (interpolationMode == OrbitInterpolationMode::FineV2CubicHermite)
	{
		return evaluateCubicHermite(stateVectors, azimuthTime, nullptr, &velocity) ? 0 : -1;
	}
	if (newStateVectors.cols != 7 || newStateVectors.rows < 2 || !isOrbitUpdated)
	{
		fprintf(stderr, "getVelocity(): input check failed!\n");
		return -1;
	}
	if (azimuthTime < newStateVectors.at<double>(0, 0) || azimuthTime > newStateVectors.at<double>(newStateVectors.rows - 1, 0))
	{
		fprintf(stderr, "getVelocity(): azimuthTime out of legal range\n");
		return -1;
	}
	int i0, iN;
	if (newStateVectors.rows <= nv) {
		i0 = 0;
		iN = newStateVectors.rows - 1;
	}
	else {
		i0 = std::max((int)((azimuthTime - newStateVectors.at<double>(0, 0)) / dt) - nv / 2 + 1, 0);
		iN = std::min(i0 + nv - 1, newStateVectors.rows - 1);
		i0 = (iN < newStateVectors.rows - 1 ? i0 : iN - nv + 1);
	}
	velocity.vx = 0.0;
	velocity.vy = 0.0;
	velocity.vz = 0.0;
	for (int i = i0; i <= iN; ++i) {
		double weight = 1.0;
		for (int j = i0; j <= iN; ++j) {
			if (j != i) {
				double time2 = newStateVectors.at<double>(j, 0);
				weight *= (azimuthTime - time2) / (newStateVectors.at<double>(i, 0) - time2);
			}
		}
		velocity.vx += weight * newStateVectors.at<double>(i, 4);
		velocity.vy += weight * newStateVectors.at<double>(i, 5);
		velocity.vz += weight * newStateVectors.at<double>(i, 6);
	}
	return 0;
}

int orbitStateVectors::getOrbitData(double time, OSV* osv)
{
	if (interpolationMode == OrbitInterpolationMode::FineV2CubicHermite)
	{
		if (!osv)
		{
			return -1;
		}
		Position position;
		Velocity velocity;
		if (!evaluateCubicHermite(stateVectors, time, &position, &velocity))
		{
			return -1;
		}
		osv->time = time;
		osv->x = position.x;
		osv->y = position.y;
		osv->z = position.z;
		osv->vx = velocity.vx;
		osv->vy = velocity.vy;
		osv->vz = velocity.vz;
		return 0;
	}
	if (!osv || !std::isfinite(time) || time < 0 || isOrbitUpdated ||
		!hasValidOrbitStateVectors(stateVectors, polyDegree + 1))
	{
		fprintf(stderr, "getOrbitData(): input check failed!\n");
		return -1;
	}
	int ret;
	int numVectors = stateVectors.rows;
	double t0 = stateVectors.at<double>(0, 0);
	double tN = stateVectors.at<double>(numVectors - 1, 0);

	int numVecPolyFit = polyDegree + 1; //4;
	int halfNumVecPolyFit = numVecPolyFit / 2;
	Mat vectorIndices = Mat::zeros(1, numVecPolyFit, CV_32S);
	int vecIdx = (int)((time - t0) / (tN - t0) * (numVectors - 1));
	if (vecIdx <= halfNumVecPolyFit - 1) {
		for (int i = 0; i < numVecPolyFit; i++) {
			vectorIndices.at<int>(0, i) = i;
		}
	}
	else if (vecIdx >= numVectors - halfNumVecPolyFit) {
		for (int i = 0; i < numVecPolyFit; i++) {
			vectorIndices.at<int>(0, i) = numVectors - numVecPolyFit + i;
		}
	}
	else {
		for (int i = 0; i < numVecPolyFit; i++) {
			vectorIndices.at<int>(0, i) = vecIdx - halfNumVecPolyFit + 1 + i;
		}
	}

	Mat timeArray = Mat::zeros(numVecPolyFit, 1, CV_64F);
	Mat xPosArray = Mat::zeros(numVecPolyFit, 1, CV_64F);
	Mat yPosArray = Mat::zeros(numVecPolyFit, 1, CV_64F);
	Mat zPosArray = Mat::zeros(numVecPolyFit, 1, CV_64F);
	Mat xVelArray = Mat::zeros(numVecPolyFit, 1, CV_64F);
	Mat yVelArray = Mat::zeros(numVecPolyFit, 1, CV_64F);
	Mat zVelArray = Mat::zeros(numVecPolyFit, 1, CV_64F);


	for (int i = 0; i < numVecPolyFit; i++) {
		timeArray.at<double>(i, 0) = stateVectors.at<double>(vectorIndices.at<int>(0, i), 0) - t0;
		xPosArray.at<double>(i, 0) = stateVectors.at<double>(vectorIndices.at<int>(0, i), 1);
		yPosArray.at<double>(i, 0) = stateVectors.at<double>(vectorIndices.at<int>(0, i), 2);
		zPosArray.at<double>(i, 0) = stateVectors.at<double>(vectorIndices.at<int>(0, i), 3);
		xVelArray.at<double>(i, 0) = stateVectors.at<double>(vectorIndices.at<int>(0, i), 4);
		yVelArray.at<double>(i, 0) = stateVectors.at<double>(vectorIndices.at<int>(0, i), 5);
		zVelArray.at<double>(i, 0) = stateVectors.at<double>(vectorIndices.at<int>(0, i), 6);

	}
	Mat A, xPosCoeff, yPosCoeff, zPosCoeff, xVelCoeff, yVelCoeff, zVelCoeff;
	Utils::createVandermondeMatrix(timeArray, A, polyDegree);
	ret = Utils::polyFit(A, xPosArray, xPosCoeff);
	if (ret < 0) return -1;
	ret = Utils::polyFit(A, yPosArray, yPosCoeff);
	if (ret < 0) return -1;
	ret = Utils::polyFit(A, zPosArray, zPosCoeff);
	if (ret < 0) return -1;
	ret = Utils::polyFit(A, xVelArray, xVelCoeff);
	if (ret < 0) return -1;
	ret = Utils::polyFit(A, yVelArray, yVelCoeff);
	if (ret < 0) return -1;
	ret = Utils::polyFit(A, zVelArray, zVelCoeff);
	if (ret < 0) return -1;
	double normalizedTime = time - t0;

	osv->time = time;
	ret = Utils::polyVal(xPosCoeff, normalizedTime, &osv->x);
	if (ret < 0) return -1;
	ret = Utils::polyVal(yPosCoeff, normalizedTime, &osv->y);
	if (ret < 0) return -1;
	ret = Utils::polyVal(zPosCoeff, normalizedTime, &osv->z);
	if (ret < 0) return -1;
	ret = Utils::polyVal(xVelCoeff, normalizedTime, &osv->vx);
	if (ret < 0) return -1;
	ret = Utils::polyVal(yVelCoeff, normalizedTime, &osv->vy);
	if (ret < 0) return -1;
	ret = Utils::polyVal(zVelCoeff, normalizedTime, &osv->vz);
	if (ret < 0) return -1;
	return 0;
}

int orbitStateVectors::applyOrbit(ProgressCallback progressCallback, void* userData)
{
	if (progressCallback && !progressCallback(0, "Updating orbit state vectors...", userData)) return -2;
	if (!std::isfinite(startTime) || !std::isfinite(stopTime) || !(stopTime > startTime))
	{
		fprintf(stderr, "applyOrbit(): scene time range is invalid!\n");
		return -1;
	}
	if (isOrbitUpdated)
	{
		if (!hasValidOrbitStateVectors(newStateVectors, 2))
		{
			fprintf(stderr, "applyOrbit(): updated orbit state vectors are invalid!\n");
			return -1;
		}
		return 0;
	}
	if (!hasValidOrbitStateVectors(stateVectors, polyDegree + 1))
	{
		fprintf(stderr, "applyOrbit(): source orbit state vectors are insufficient!\n");
		return -1;
	}
	double delta_t = 1.0;//1.0s
	this->dt = delta_t;
	double extra = 10.0;//10.0s
	double start = startTime - extra;
	double stop = stopTime + extra;
	int numVectors = (int)((stop - start) / delta_t);
	if (numVectors < 2)
	{
		fprintf(stderr, "applyOrbit(): generated orbit state vectors are insufficient!\n");
		return -1;
	}
	OSV osv;
	Mat newStateVectors = Mat::zeros(numVectors, 7, CV_64F);
	int ret;
	for (int i = 0; i < numVectors; i++)
	{
		if (progressCallback && i % 16 == 0 &&
			!progressCallback(i * 100 / std::max(1, numVectors), "Updating orbit state vectors...", userData)) return -2;
		ret = getOrbitData(start + (double)i * delta_t, &osv);
		if (ret < 0) return -1;
		newStateVectors.at<double>(i, 0) = start + (double)i * delta_t;
		newStateVectors.at<double>(i, 1) = osv.x;
		newStateVectors.at<double>(i, 2) = osv.y;
		newStateVectors.at<double>(i, 3) = osv.z;
		newStateVectors.at<double>(i, 4) = osv.vx;
		newStateVectors.at<double>(i, 5) = osv.vy;
		newStateVectors.at<double>(i, 6) = osv.vz;


	}
	newStateVectors.copyTo(this->newStateVectors);
	isOrbitUpdated = true;
	if (progressCallback && !progressCallback(100, "Orbit state vectors updated.", userData)) return -2;
	return 0;
}

bool orbitStateVectors::findZeroDopplerTime(
	orbitStateVectors& stateVectors,
	const Position& groundPosition,
	double wavelength,
	double time_interval,
	double dopplerFrequency,
	double& zeroDopplerTime,
	double& distance,
	double dopplerThreshold)
{
	if (!hasValidOrbitStateVectors(stateVectors.newStateVectors, 2) ||
		!std::isfinite(groundPosition.x) || !std::isfinite(groundPosition.y) ||
		!std::isfinite(groundPosition.z) ||
		!std::isfinite(wavelength) || wavelength <= 0.0 ||
		!std::isfinite(time_interval) || time_interval <= 0.0 ||
		!std::isfinite(dopplerFrequency) || !std::isfinite(dopplerThreshold) || dopplerThreshold <= 0.0)
	{
		return false;
	}
	int numOrbitVec = stateVectors.newStateVectors.rows;
	double firstVecTime = 0.0;
	double secondVecTime = 0.0;
	double firstVecFreq = 0.0;
	double secondVecFreq = 0.0;
	double currentFreq, xdiff, ydiff, zdiff;

	for (int ii = 0; ii < numOrbitVec; ii++) {
		Position orb_pos(stateVectors.newStateVectors.at<double>(ii, 1), stateVectors.newStateVectors.at<double>(ii, 2),
			stateVectors.newStateVectors.at<double>(ii, 3));
		Velocity orb_vel(stateVectors.newStateVectors.at<double>(ii, 4), stateVectors.newStateVectors.at<double>(ii, 5),
			stateVectors.newStateVectors.at<double>(ii, 6));
		xdiff = groundPosition.x - orb_pos.x;
		ydiff = groundPosition.y - orb_pos.y;
		zdiff = groundPosition.z - orb_pos.z;
		double dist = sqrt(xdiff * xdiff + ydiff * ydiff + zdiff * zdiff);
		if (!std::isfinite(dist) || dist <= 0.0)
		{
			return false;
		}
		currentFreq = 2.0 * (xdiff * orb_vel.vx + ydiff * orb_vel.vy + zdiff * orb_vel.vz) / (wavelength * dist);
		if (!std::isfinite(currentFreq))
		{
			return false;
		}
		if (ii == 0 || (firstVecFreq - dopplerFrequency) * (currentFreq - dopplerFrequency) > 0) {
			firstVecTime = stateVectors.newStateVectors.at<double>(ii, 0);
			firstVecFreq = currentFreq;
		}
		else {
			secondVecTime = stateVectors.newStateVectors.at<double>(ii, 0);
			secondVecFreq = currentFreq;
			break;
		}
	}

	if ((firstVecFreq - dopplerFrequency) * (secondVecFreq - dopplerFrequency) >= 0.0) {
		return false;
	}

	double lowerBoundTime = firstVecTime;
	double upperBoundTime = secondVecTime;
	double lowerBoundFreq = firstVecFreq;
	double upperBoundFreq = secondVecFreq;
	double midTime, midFreq;
	double diffTime = fabs(upperBoundTime - lowerBoundTime);
	double absLineTimeInterval = time_interval;

	int totalIterations = (int)(diffTime / absLineTimeInterval) + 1;
	int numIterations = 0;
	Position pos; Velocity vel;
	while (diffTime > absLineTimeInterval * 0.01 && numIterations <= totalIterations) {
		midTime = (upperBoundTime + lowerBoundTime) / 2.0;
		if (stateVectors.getPosition(midTime, pos) < 0 || stateVectors.getVelocity(midTime, vel) < 0)
		{
			return false;
		}
		xdiff = groundPosition.x - pos.x;
		ydiff = groundPosition.y - pos.y;
		zdiff = groundPosition.z - pos.z;
		double dist = sqrt(xdiff * xdiff + ydiff * ydiff + zdiff * zdiff);
		if (!std::isfinite(dist) || dist <= 0.0)
		{
			return false;
		}
		midFreq = 2.0 * (xdiff * vel.vx + ydiff * vel.vy + zdiff * vel.vz) / (wavelength * dist);
		if (!std::isfinite(midFreq))
		{
			return false;
		}
		if ((midFreq - dopplerFrequency) * (lowerBoundFreq - dopplerFrequency) > 0.0) {
			lowerBoundTime = midTime;
			lowerBoundFreq = midFreq;
		}
		else if ((midFreq - dopplerFrequency) * (upperBoundFreq - dopplerFrequency) > 0.0) {
			upperBoundTime = midTime;
			upperBoundFreq = midFreq;
		}
		else if (fabs(midFreq - dopplerFrequency) < dopplerThreshold) {
			lowerBoundTime = midTime;
			break;
		}
		diffTime = fabs(upperBoundTime - lowerBoundTime);
		numIterations++;
	}

	const double frequencySpan = upperBoundFreq - lowerBoundFreq;
	if (!std::isfinite(frequencySpan) || frequencySpan == 0.0)
	{
		return false;
	}
	zeroDopplerTime = lowerBoundTime + (dopplerFrequency - lowerBoundFreq) * (upperBoundTime - lowerBoundTime) / frequencySpan;
	if (!std::isfinite(zeroDopplerTime) || stateVectors.getPosition(zeroDopplerTime, pos) < 0)
	{
		return false;
	}
	xdiff = groundPosition.x - pos.x;
	ydiff = groundPosition.y - pos.y;
	zdiff = groundPosition.z - pos.z;
	distance = sqrt(xdiff * xdiff + ydiff * ydiff + zdiff * zdiff);
	return std::isfinite(distance) && distance > 0.0;
}






