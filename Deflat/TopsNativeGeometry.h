#pragma once

#include "..\include\Deflat.h"

#include <memory>

// Internal v5 geometry kernel. It owns the two prepared Core orbit objects;
// no H5, UI, geolocation polynomial, or registration mutation is permitted
// below this boundary.
class TopsNativeGeometry
{
public:
    struct SampleClosure
    {
        double masterRangeErrorBound = 0.0;
        double masterPointLastStep = 0.0;
        double slaveRangeLastStep = 0.0;
        double slaveRangeErrorBound = 0.0;
    };

    TopsNativeGeometry(const TopsFepV5Orbit& masterOrbit,
                       const TopsFepV5Orbit& slaveOrbit,
                       const TopsFepV5Options& options);
    ~TopsNativeGeometry();

    bool prepare(std::string* failureReason = nullptr);
    bool masterState(double time, Position& position, Velocity& velocity) const;
	bool slaveState(double time, Position& position, Velocity& velocity) const;
    bool solveMasterH0Point(const Position& satellite,
                            const Velocity& velocity,
                            double rhoMaster,
                            const Position* rangeSeed,
                            Position& point,
                            SampleClosure& closure,
                            TopsFepV5BurstStatistics& statistics) const;
    // Solves the master range/Doppler/ellipsoid (RDE) intersection on the
    // FEP-selected orbit at an explicit ellipsoidal reference height.  This
    // is deliberately adjacent to the h=0 entry point so consumers cannot
    // reconstruct a separate orbit or infer a height from a GCP fallback.
    bool solveMasterReferenceHeightPoint(const Position& satellite,
                                         const Velocity& velocity,
                                         double rhoMaster,
                                         double referenceEllipsoidHeight,
                                         const Position* rangeSeed,
                                         Position& point,
                                         SampleClosure& closure,
                                         TopsFepV5BurstStatistics& statistics) const;
    bool solveSlaveZeroDoppler(const Position& point,
                               double registrationTimeSeed,
                               double& zeroDopplerTime,
                               double& rhoSlave,
                               SampleClosure& closure,
                               TopsFepV5BurstStatistics& statistics) const;

private:
    bool evaluateSlaveDoppler(const Position& point, double time,
                              double& normalizedDoppler, double& range) const;
	bool slaveCoverage(double& firstGps, double& lastGps) const;
	struct RawStateVecCubicInterpolator;

    const TopsFepV5Orbit& m_masterOrbit;
    const TopsFepV5Orbit& m_slaveOrbit;
    const TopsFepV5Options& m_options;
    std::unique_ptr<orbitStateVectors> m_masterFine;
    std::unique_ptr<orbitStateVectors> m_slaveFine;
    std::unique_ptr<RawStateVecCubicInterpolator> m_masterRaw;
    std::unique_ptr<RawStateVecCubicInterpolator> m_slaveRaw;
};
