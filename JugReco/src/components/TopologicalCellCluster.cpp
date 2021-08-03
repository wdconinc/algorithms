/*
 *  Topological Cell Clustering Algorithm for Sampling Calorimetry
 *  1. group all the adjacent modules
 *  2. TODO split local maxima (seems no need for Imaging Calorimeter with extremely fine
 * granularity)
 *
 *  Author: Chao Peng (ANL), 04/06/2021
 *  References: https://arxiv.org/pdf/1603.02934.pdf
 *
 */
#include "fmt/format.h"
#include <algorithm>

#include "Gaudi/Property.h"
#include "GaudiAlg/GaudiAlgorithm.h"
#include "GaudiAlg/GaudiTool.h"
#include "GaudiAlg/Transformer.h"
#include "GaudiKernel/PhysicalConstants.h"
#include "GaudiKernel/RndmGenerators.h"
#include "GaudiKernel/ToolHandle.h"

#include "DD4hep/BitFieldCoder.h"
#include "DDRec/CellIDPositionConverter.h"
#include "DDRec/Surface.h"
#include "DDRec/SurfaceManager.h"

// FCCSW
#include "JugBase/DataHandle.h"
#include "JugBase/IGeoSvc.h"

// Event Model related classes
#include "eicd/CalorimeterHitCollection.h"
#include "eicd/ClusterCollection.h"

using namespace Gaudi::Units;

namespace Jug::Reco {

  /** Topological Cell Clustering Algorithm for Sampling Calorimetry.
   *
   *  1. group all the adjacent modules
   *  2. TODO split local maxima (seems no need for Imaging Calorimeter with extremely fine
   * granularity)
   *
   * \ingroup reco
   */
  class TopologicalCellCluster : public GaudiAlgorithm {
  public:
    // maximum difference in layer numbers that can be considered as neighbours
    Gaudi::Property<int> m_adjLayerDiff{this, "adjLayerDiff", 1};
    // geometry service name
    Gaudi::Property<std::string> m_geoSvcName{this, "geoServiceName", "GeoSvc"};
    // name of readout class
    Gaudi::Property<std::string> m_readout{this, "readoutClass", ""};
    // name of layer field in readout
    Gaudi::Property<std::string> m_layerField{this, "layerField", "layer"};
    // name of sector field in readout
    Gaudi::Property<std::string> m_sectorField{this, "sectorField", "sector"};
    // maximum distance of local (x, y) to be considered as neighbors at the same layer
    Gaudi::Property<std::vector<double>> u_localRanges{this, "localRanges", {1.0 * mm, 1.0 * mm}};
    // maximum distance of global (eta, phi) to be considered as neighbors at different layers
    Gaudi::Property<std::vector<double>> u_adjLayerRanges{
        this, "adjLayerRanges", {0.01 * M_PI, 0.01 * M_PI}};
    // maximum global distance to be considered as neighbors in different sectors
    Gaudi::Property<double> m_adjSectorDist{this, "adjSectorDist", 1.0 * cm};
    // minimum cluster center energy (to be considered as a seed for cluster)
    // @TODO One can not simply find a center by edep with extremely fine granularity.
    // Projecting to (eta, phi) with crude pixel size may help determine the center edep, which can
    // happen in the following reconstruction step
    Gaudi::Property<double> m_minClusterCenterEdep{this, "minClusterCenterEdep", 50 * keV};
    // input hits collection
    DataHandle<eic::CalorimeterHitCollection> m_inputHitCollection{"inputHitCollection",
                                                                   Gaudi::DataHandle::Reader, this};
    // output cluster collection
    DataHandle<eic::ClusterCollection> m_outputClusterCollection{"outputClusterCollection",
                                                                 Gaudi::DataHandle::Writer, this};
    // output split hits collection
    // @TODO not implemented, as with extreme fine granularity, there is no need to split hits
    // DataHandle<eic::CalorimeterHitCollection>
    //     m_splitHitCollection{"splitHitCollection", Gaudi::DataHandle::Writer, this};

    SmartIF<IGeoSvc> m_geoSvc;
    // visit readout fields
    dd4hep::BitFieldCoder* id_dec;
    size_t                 sector_idx, layer_idx;

    // ill-formed: using GaudiAlgorithm::GaudiAlgorithm;
    TopologicalCellCluster(const std::string& name, ISvcLocator* svcLoc)
        : GaudiAlgorithm(name, svcLoc)
    {
      declareProperty("inputHitCollection", m_inputHitCollection, "");
      declareProperty("outputClusterCollection", m_outputClusterCollection, "");
    }

    StatusCode initialize() override
    {
      if (GaudiAlgorithm::initialize().isFailure()) {
        return StatusCode::FAILURE;
      }

      // check local clustering range
      if (u_localRanges.size() < 2) {
        error() << "Need 2-dimensional ranges for same-layer clustering, only have "
                << u_localRanges.size() << endmsg;
        return StatusCode::FAILURE;
      }
      info() << "Same layer hits group ranges"
             << " (" << u_localRanges[0] / mm << " mm, " << u_localRanges[1] / mm << " mm)"
             << endmsg;

      // check adjacent layer clustering range
      if (u_adjLayerRanges.size() < 2) {
        error() << "Need 2-dimensional ranges for adjacent-layer clustering, only have "
                << u_adjLayerRanges.size() << endmsg;
        return StatusCode::FAILURE;
      }
      info() << "Same layer hits group ranges"
             << " (" << u_adjLayerRanges[0] / M_PI * 1000. << " mrad, "
             << u_adjLayerRanges[1] / M_PI * 1000. << " mrad)" << endmsg;

      // check geometry service
      m_geoSvc = service(m_geoSvcName);
      if (!m_geoSvc) {
        error() << "Unable to locate Geometry Service. "
                << "Make sure you have GeoSvc and SimSvc in the right order in the configuration."
                << endmsg;
        return StatusCode::FAILURE;
      }

      if (m_readout.value().empty()) {
        error() << "readoutClass is not provided, it is needed to know the fields in readout ids"
                << endmsg;
        return StatusCode::FAILURE;
      }

      try {
        id_dec     = m_geoSvc->detector()->readout(m_readout).idSpec().decoder();
        sector_idx = id_dec->index(m_sectorField);
        layer_idx  = id_dec->index(m_layerField);
      } catch (...) {
        error() << "Failed to load ID decoder for " << m_readout << endmsg;
        return StatusCode::FAILURE;
      }

      return StatusCode::SUCCESS;
    }

    StatusCode execute() override
    {
      // input collections
      const auto& hits = *m_inputHitCollection.get();
      // Create output collections
      auto& clusters = *m_outputClusterCollection.createAndPut();

      // group neighboring hits
      std::vector<bool>                             visits(hits.size(), false);
      std::vector<std::vector<eic::CalorimeterHit>> groups;
      for (size_t i = 0; i < hits.size(); ++i) {
        // already in a group, or not energetic enough to form a cluster
        if (visits[i] || hits[i].energy() < m_minClusterCenterEdep) {
          continue;
        }
        groups.emplace_back();
        // create a new group, and group all the neighboring hits
        dfs_group(groups.back(), i, hits, visits);
      }
      debug() << "we have " << groups.size() << " groups of hits" << endmsg;

      for (const auto& group : groups) {
        auto cl = clusters.create();
      }

      return StatusCode::SUCCESS;
    }

  private:
    template <typename T>
    static inline T pow2(const T& x)
    {
      return x * x;
    }

    // helper function to group hits
    bool is_neighbor(const eic::ConstCalorimeterHit& h1, const eic::ConstCalorimeterHit& h2) const
    {
      // we will merge different sectors later using global positions
      int s1 = id_dec->get(h1.cellID(), sector_idx);
      int s2 = id_dec->get(h2.cellID(), sector_idx);
      if (s1 != s2) {
        return std::sqrt(pow2(h1.position().x - h2.position().x) + pow2(h1.position().y - h2.position().y) + pow2(h1.position().z - h2.position().z)) <=
               m_adjSectorDist;
      }

      int l1 = id_dec->get(h1.cellID(), layer_idx);
      int l2 = id_dec->get(h2.cellID(), layer_idx);

      // layer check
      int ldiff = std::abs(l1 - l2);
      // same layer, check local positions
      if (!ldiff) {
        return (std::abs(h1.local().x - h2.local().x) <= u_localRanges[0]) &&
               (std::abs(h1.local().y - h2.local().y) <= u_localRanges[1]);
      } else if (ldiff <= m_adjLayerDiff) {
        double eta1, phi1, r1, eta2, phi2, r2;
        calc_eta_phi_r(h1.position().x, h1.position().y, h1.position().z, eta1, phi1, r1);
        calc_eta_phi_r(h2.position().x, h2.position().y, h2.position().z, eta2, phi2, r2);

        return (std::abs(eta1 - eta2) <= u_adjLayerRanges[0]) &&
               (std::abs(phi1 - phi2) <= u_adjLayerRanges[1]);
      }

      // not in adjacent layers
      return false;
    }

    static void calc_eta_phi_r(double x, double y, double z, double& eta, double& phi, double& r)
    {
      r            = std::sqrt(x * x + y * y + z * z);
      phi          = std::atan2(y, x);
      double theta = std::acos(z / r);
      eta          = -std::log(std::tan(theta / 2.));
    }

    // grouping function with Depth-First Search
    void dfs_group(std::vector<eic::CalorimeterHit>& group, int idx,
                   const eic::CalorimeterHitCollection& hits, std::vector<bool>& visits) const
    {
      const eic::CalorimeterHit hit{hits[idx].cellID(),    hits[idx].clusterID(),
                                    hits[idx].layerID(),   hits[idx].sectorID(),
                                    hits[idx].energy(),    hits[idx].time(),
                                    hits[idx].position(),  hits[idx].local(),
                                    hits[idx].dimension(), 1};
      group.push_back(hit);
      visits[idx] = true;
      for (size_t i = 0; i < hits.size(); ++i) {
        // visited, or not a neighbor
        if (visits[i] || !is_neighbor(hit, hits[i])) {
          continue;
        }
        dfs_group(group, i, hits, visits);
      }
    }
  };

  DECLARE_COMPONENT(TopologicalCellCluster)

} // namespace Jug::Reco

