#pragma once

#include "routing/base/astar_algorithm.hpp"
#include "routing/base/astar_graph.hpp"
#include "routing/base/astar_vertex_data.hpp"
#include "routing/edge_estimator.hpp"
#include "routing/geometry.hpp"
#include "routing/joint.hpp"
#include "routing/joint_index.hpp"
#include "routing/joint_segment.hpp"
#include "routing/restrictions_serialization.hpp"
#include "routing/road_access.hpp"
#include "routing/road_index.hpp"
#include "routing/road_point.hpp"
#include "routing/routing_options.hpp"
#include "routing/segment.hpp"

#include "geometry/point2d.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace routing
{
bool IsUTurn(Segment const & u, Segment const & v);

enum class WorldGraphMode;

class IndexGraph final
{
public:
  // AStarAlgorithm types aliases:
  using Vertex = Segment;
  using Edge = SegmentEdge;
  using Weight = RouteWeight;

  template <typename VertexType>
  using Parents = typename AStarGraph<VertexType, void, void>::Parents;
  
  using Restrictions = std::unordered_map<uint32_t, std::vector<std::vector<uint32_t>>>;

  IndexGraph() = default;
  IndexGraph(std::shared_ptr<Geometry> geometry, std::shared_ptr<EdgeEstimator> estimator,
             RoutingOptions routingOptions = RoutingOptions());

  inline static Parents<Segment> kEmptyParentsSegments = {};
  // Put outgoing (or ingoing) egdes for segment to the 'edges' vector.
  void GetEdgeList(astar::VertexData<Segment, RouteWeight> const & vertexData, bool isOutgoing,
                   bool useRoutingOptions, std::vector<SegmentEdge> & edges,
                   Parents<Segment> const & parents = kEmptyParentsSegments);
  void GetEdgeList(Segment const & segment, bool isOutgoing, bool useRoutingOptions,
                   std::vector<SegmentEdge> & edges,
                   Parents<Segment> const & parents = kEmptyParentsSegments);

  void GetEdgeList(astar::VertexData<JointSegment, RouteWeight> const & parentVertexData,
                   Segment const & parent, bool isOutgoing, std::vector<JointEdge> & edges,
                   std::vector<RouteWeight> & parentWeights, Parents<JointSegment> & parents);
  void GetEdgeList(JointSegment const & parentJoint, Segment const & parent, bool isOutgoing,
                   std::vector<JointEdge> & edges, std::vector<RouteWeight> & parentWeights,
                   Parents<JointSegment> & parents);

  std::optional<JointEdge> GetJointEdgeByLastPoint(Segment const & parent,
                                                   Segment const & firstChild, bool isOutgoing,
                                                   uint32_t lastPoint);

  Joint::Id GetJointId(RoadPoint const & rp) const { return m_roadIndex.GetJointId(rp); }

  Geometry & GetGeometry() { return *m_geometry; }
  bool IsRoad(uint32_t featureId) const { return m_roadIndex.IsRoad(featureId); }
  RoadJointIds const & GetRoad(uint32_t featureId) const { return m_roadIndex.GetRoad(featureId); }

  RoadAccess::Type GetAccessType(Segment const & segment) const
  {
    return m_roadAccess.GetAccess(segment.GetFeatureId());
  }

  uint32_t GetNumRoads() const { return m_roadIndex.GetSize(); }
  uint32_t GetNumJoints() const { return m_jointIndex.GetNumJoints(); }
  uint32_t GetNumPoints() const { return m_jointIndex.GetNumPoints(); }

  void Build(uint32_t numJoints);
  void Import(std::vector<Joint> const & joints);

  void SetRestrictions(RestrictionVec && restrictions);
  void SetUTurnRestrictions(std::vector<RestrictionUTurn> && noUTurnRestrictions);
  void SetRoadAccess(RoadAccess && roadAccess);

  void PushFromSerializer(Joint::Id jointId, RoadPoint const & rp)
  {
    m_roadIndex.PushFromSerializer(jointId, rp);
  }

  template <typename F>
  void ForEachRoad(F && f) const
  {
    m_roadIndex.ForEachRoad(std::forward<F>(f));
  }

  template <typename F>
  void ForEachPoint(Joint::Id jointId, F && f) const
  {
    m_jointIndex.ForEachPoint(jointId, std::forward<F>(f));
  }

  bool IsJoint(RoadPoint const & roadPoint) const;
  bool IsJointOrEnd(Segment const & segment, bool fromStart);
  void GetLastPointsForJoint(std::vector<Segment> const & children, bool isOutgoing,
                             std::vector<uint32_t> & lastPoints);

  WorldGraphMode GetMode() const;
  ms::LatLon const & GetPoint(Segment const & segment, bool front)
  {
    return GetGeometry().GetRoad(segment.GetFeatureId()).GetPoint(segment.GetPointId(front));
  }

  /// \brief Check, that we can go to |currentFeatureId|.
  /// We pass |parentFeatureId| and don't use |parent.GetFeatureId()| because
  /// |parent| can be fake sometimes but |parentFeatureId| is almost non-fake.
  template <typename ParentVertex>
  bool IsRestricted(ParentVertex const & parent,
                    uint32_t parentFeatureId,
                    uint32_t currentFeatureId, bool isOutgoing,
                    Parents<ParentVertex> const & parents) const;

  bool IsUTurnAndRestricted(Segment const & parent, Segment const & child, bool isOutgoing) const;

  RouteWeight CalculateEdgeWeight(EdgeEstimator::Purpose purpose, bool isOutgoing,
                                  Segment const & from, Segment const & to);

private:
  void GetEdgeListImpl(astar::VertexData<Segment, RouteWeight> const & vertexData, bool isOutgoing,
                       bool useRoutingOptions, bool useAccessConditional,
                       std::vector<SegmentEdge> & edges, Parents<Segment> const & parents);

  void GetEdgeListImpl(astar::VertexData<JointSegment, RouteWeight> const & parentVertexData,
                       Segment const & parent, bool isOutgoing, bool useAccessConditional,
                       std::vector<JointEdge> & edges, std::vector<RouteWeight> & parentWeights,
                       Parents<JointSegment> & parents);

  void GetNeighboringEdges(astar::VertexData<Segment, RouteWeight> const & fromVertexData,
                           RoadPoint const & rp, bool isOutgoing, bool useRoutingOptions,
                           std::vector<SegmentEdge> & edges, Parents<Segment> const & parents,
                           bool useAccessConditional);
  void GetNeighboringEdge(astar::VertexData<Segment, RouteWeight> const & fromVertexData,
                          Segment const & to, bool isOutgoing, std::vector<SegmentEdge> & edges,
                          Parents<Segment> const & parents, bool useAccessConditional);

  struct PenaltyData
  {
    PenaltyData(bool passThroughAllowed, bool isFerry)
      : m_passThroughAllowed(passThroughAllowed),
        m_isFerry(isFerry) {}

    bool m_passThroughAllowed;
    bool m_isFerry;
  };

  PenaltyData GetRoadPenaltyData(Segment const & segment);
  RouteWeight GetPenalties(EdgeEstimator::Purpose purpose, Segment const & u, Segment const & v);

  void GetSegmentCandidateForRoadPoint(RoadPoint const & rp, NumMwmId numMwmId,
                                       bool isOutgoing, std::vector<Segment> & children);
  void GetSegmentCandidateForJoint(Segment const & parent, bool isOutgoing, std::vector<Segment> & children);
  void ReconstructJointSegment(astar::VertexData<JointSegment, RouteWeight> const & parentVertexData,
                               Segment const & parent,
                               std::vector<Segment> const & firstChildren,
                               std::vector<uint32_t> const & lastPointIds,
                               bool isOutgoing,
                               std::vector<JointEdge> & jointEdges,
                               std::vector<RouteWeight> & parentWeights,
                               Parents<JointSegment> const & parents);

  std::shared_ptr<Geometry> m_geometry;
  std::shared_ptr<EdgeEstimator> m_estimator;
  RoadIndex m_roadIndex;
  JointIndex m_jointIndex;

  Restrictions m_restrictionsForward;
  Restrictions m_restrictionsBackward;

  // u_turn can be in both sides of feature.
  struct UTurnEnding
  {
    bool m_atTheBegin = false;
    bool m_atTheEnd = false;
  };
  // Stored featureId and it's UTurnEnding, which shows where is
  // u_turn restriction is placed - at the beginning or at the ending of feature.
  //
  // If m_noUTurnRestrictions.count(featureId) == 0, that means, that there are no any
  // no_u_turn restriction at the feature with id = featureId.
  std::unordered_map<uint32_t, UTurnEnding> m_noUTurnRestrictions;

  RoadAccess m_roadAccess;
  RoutingOptions m_avoidRoutingOptions;
};

template <typename ParentVertex>
bool IndexGraph::IsRestricted(ParentVertex const & parent,
                              uint32_t parentFeatureId,
                              uint32_t currentFeatureId,
                              bool isOutgoing,
                              Parents<ParentVertex> const & parents) const
{
  if (parentFeatureId == currentFeatureId)
    return false;

  auto const & restrictions = isOutgoing ? m_restrictionsForward : m_restrictionsBackward;
  auto const it = restrictions.find(currentFeatureId);
  if (it == restrictions.cend())
    return false;

  std::vector<ParentVertex> parentsFromCurrent;
  // Finds the first featureId from parents, that differ from |p.GetFeatureId()|.
  auto const appendNextParent = [&parents](ParentVertex const & p, auto & parentsVector)
  {
    uint32_t prevFeatureId = p.GetFeatureId();
    uint32_t curFeatureId = prevFeatureId;

    auto nextParent = parents.end();
    auto * curParent = &p;
    while (curFeatureId == prevFeatureId)
    {
      auto const parentIt = parents.find(*curParent);
      if (parentIt == parents.cend())
        return false;

      curFeatureId = parentIt->second.GetFeatureId();
      nextParent = parentIt;
      curParent = &nextParent->second;
    }

    ASSERT(nextParent != parents.end(), ());
    parentsVector.emplace_back(nextParent->second);
    return true;
  };

  for (std::vector<uint32_t> const & restriction : it->second)
  {
    bool const prevIsParent = restriction[0] == parentFeatureId;
    if (!prevIsParent)
      continue;

    if (restriction.size() == 1)
      return true;

    // If parents are empty we process only two feature restrictions.
    if (parents.empty())
      continue;

    if (!appendNextParent(parent, parentsFromCurrent))
      continue;

    for (size_t i = 1; i < restriction.size(); ++i)
    {
      ASSERT_GREATER_OR_EQUAL(i, 1, ("Unexpected overflow."));
      if (i - 1 == parentsFromCurrent.size()
          && !appendNextParent(parentsFromCurrent.back(), parentsFromCurrent))
      {
        break;
      }

      if (parentsFromCurrent.back().GetFeatureId() != restriction[i])
        break;

      if (i + 1 == restriction.size())
        return true;
    }
  }

  return false;
}
}  // namespace routing
