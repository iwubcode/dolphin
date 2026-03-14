// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModEditor/VertexGroupDumper.h"

#include <algorithm>
#include <future>
#include <queue>
#include <random>
#include <set>
#include <utility>

#include <Eigen/Core>
#include <Eigen/Dense>
#include <Eigen/Geometry>
#include <Eigen/Sparse>
#include <Eigen/SparseCholesky>
#include <igl/remove_duplicate_vertices.h>

#include <fmt/chrono.h>
#include <fmt/format.h>
#include <tinygltf/tiny_gltf.h>

#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Common/JsonUtil.h"
#include "Common/Logging/Log.h"
#include "Common/TimeUtil.h"

#include "Core/ConfigManager.h"

#include "VideoCommon/GraphicsModEditor/GpuSkinningDataUtils.h"

#ifdef _MSC_VER
#pragma warning(disable : 4267)  // "conversion from size_t to int in libigl
#endif

namespace
{
namespace SimpleGeodesic
{

struct HeatGeodesicsData
{
  Eigen::SparseMatrix<double> L;  // Laplacian
  Eigen::SparseMatrix<double> M;  // Mass Matrix
  Eigen::SimplicialLLT<Eigen::SparseMatrix<double>> heat_solver;
  Eigen::MatrixXd V;
  Eigen::MatrixXi F;
  double t;  // Time step
  Eigen::VectorXd geo_from_center;
};

/**
 * Precompute the sparse factors for the Heat Method.
 * Returns false if the mesh is degenerate or matrices are not positive definite.
 */
bool heat_geodesics_precompute(const Eigen::MatrixXd& V, const Eigen::MatrixXi& F,
                               HeatGeodesicsData& data)
{
  int n = V.rows();
  data.V = V;
  data.F = F;

  std::vector<Eigen::Triplet<double>> L_triplets, M_triplets;
  double total_edge_len = 0;
  int edge_count = 0;

  for (int f = 0; f < F.rows(); ++f)
  {
    for (int i = 0; i < 3; ++i)
    {
      int i0 = F(f, i);
      int i1 = F(f, (i + 1) % 3);
      int i2 = F(f, (i + 2) % 3);

      Eigen::Vector3d v0 = V.row(i0), v1 = V.row(i1), v2 = V.row(i2);
      Eigen::Vector3d e1 = v1 - v0, e2 = v2 - v0;

      double area = 0.5 * e1.cross(e2).norm();
      if (area < 1e-12)
        continue;  // Skip degenerate

      // Cotangent of angle at v0
      double cot0 = e1.dot(e2) / e1.cross(e2).norm();
      double weight = 0.5 * cot0;

      // Add to off-diagonals (v1, v2)
      L_triplets.push_back({i1, i2, weight});
      L_triplets.push_back({i2, i1, weight});

      // Add to diagonals
      L_triplets.push_back({i1, i1, -weight});
      L_triplets.push_back({i2, i2, -weight});

      // Mass Matrix (Barycentric area)
      M_triplets.push_back({i0, i0, area / 3.0});

      total_edge_len += e1.norm();
      edge_count++;
    }
  }

  Eigen::SparseMatrix<double> L(n, n), M(n, n);
  L.setFromTriplets(L_triplets.begin(), L_triplets.end());
  M.setFromTriplets(M_triplets.begin(), M_triplets.end());

  // t = h^2 where h is mean edge length
  double h = total_edge_len / edge_count;
  data.t = h * h * 0.1;

  // Heat step: (M - tL)
  Eigen::SparseMatrix<double> A_heat = M - data.t * L;
  data.heat_solver.compute(A_heat);

  // Calculate geo_from_center
  Eigen::Vector3d centroid = V.colwise().mean();
  int center_v = 0;
  double min_dist = 1e9;
  for (int i = 0; i < V.rows(); ++i)
  {
    double d = (V.row(i).transpose() - centroid).norm();
    if (d < min_dist)
    {
      min_dist = d;
      center_v = i;
    }
  }

  Eigen::VectorXd rhs = Eigen::VectorXd::Zero(V.rows());
  rhs(center_v) = 1.0;
  data.geo_from_center = data.heat_solver.solve(rhs);

  // Normalize geo_from_center to a stable [0,1] range to prevent scale domination
  {
    double minv = data.geo_from_center.minCoeff();
    data.geo_from_center.array() -= minv;
    double maxv = data.geo_from_center.maxCoeff();
    if (maxv > 1e-12)
      data.geo_from_center /= maxv;
  }

  data.L = L;
  data.M = M;
  return data.heat_solver.info() == Eigen::Success;
}

}  // namespace SimpleGeodesic

// Post-process bone assignments to ensure contiguous regions
void PostProcessBoneGroupsByConnectivity(std::vector<int>& assignments, const Eigen::MatrixXi& F,
                                         int numVertices, int numBones)
{
  // Build adjacency
  std::vector<std::vector<int>> adj(numVertices);
  for (int i = 0; i < F.rows(); ++i)
  {
    for (int j = 0; j < 3; ++j)
    {
      int u = F(i, j);
      int v = F(i, (j + 1) % 3);
      adj[u].push_back(v);
      adj[v].push_back(u);
    }
  }

  // For each bone, find contiguous islands
  std::vector<int> islandID(numVertices, -1);
  std::vector<std::vector<int>> islands;
  std::vector<int> islandBoneType;

  std::vector<bool> visited(numVertices, false);
  for (int i = 0; i < numVertices; ++i)
  {
    if (visited[i])
      continue;

    int currentBone = assignments[i];
    std::vector<int> component;
    std::queue<int> q;
    q.push(i);
    visited[i] = true;

    while (!q.empty())
    {
      int u = q.front();
      q.pop();
      component.push_back(u);
      islandID[u] = (int)islands.size();
      for (int v : adj[u])
      {
        if (!visited[v] && assignments[v] == currentBone)
        {
          visited[v] = true;
          q.push(v);
        }
      }
    }
    islandBoneType.push_back(currentBone);
    islands.push_back(component);
  }

  // For each bone, keep only the largest contiguous island
  std::vector<int> alphaIslandForBone(numBones, -1);
  std::vector<size_t> maxSizes(numBones, 0);
  for (std::size_t i = 0; i < islands.size(); i++)
  {
    int b = islandBoneType[i];
    if (islands[i].size() > maxSizes[b])
    {
      maxSizes[b] = islands[i].size();
      alphaIslandForBone[b] = i;
    }
  }

  // Snap orphans: If you're not in your bone's alpha island, assign to neighbor's bone
  for (int i = 0; i < numVertices; i++)
  {
    int myIsland = islandID[i];
    int myBone = assignments[i];

    if (myIsland != alphaIslandForBone[myBone])
    {
      // Find a neighbor in a main island
      for (int neighbor : adj[i])
      {
        int nIsland = islandID[neighbor];
        int nBone = assignments[neighbor];
        if (nIsland == alphaIslandForBone[nBone])
        {
          assignments[i] = nBone;
          break;
        }
      }
    }
  }
}

void MergeSmallAdjacentGroups(std::vector<int>& assignments, const Eigen::MatrixXi& F,
                              const SimpleGeodesic::HeatGeodesicsData& heat_data, int numBones)
{
  // For each pair of bones, if they're adjacent and geodesic distance is low, merge
  for (int b1 = 0; b1 < numBones; ++b1)
  {
    for (int b2 = b1 + 1; b2 < numBones; ++b2)
    {
      // Find adjacency
      bool adjacent = false;
      for (int i = 0; i < F.rows(); ++i)
      {
        for (int j = 0; j < 3; ++j)
        {
          int u = F(i, j);
          int v = F(i, (j + 1) % 3);
          if ((assignments[u] == b1 && assignments[v] == b2) ||
              (assignments[u] == b2 && assignments[v] == b1))
          {
            adjacent = true;
            break;
          }
        }
        if (adjacent)
          break;
      }
      if (!adjacent)
        continue;

      // Compute geodesic distance between groups
      std::vector<int> group1, group2;
      for (int i = 0; i < assignments.size(); ++i)
      {
        if (assignments[i] == b1)
          group1.push_back(i);
        if (assignments[i] == b2)
          group2.push_back(i);
      }
      if (group1.empty() || group2.empty())
        continue;

      // Use heat_data to compute geodesic distance
      Eigen::VectorXd rhs = Eigen::VectorXd::Zero(heat_data.V.rows());
      for (int idx : group1)
        rhs(idx) = 1.0;
      Eigen::VectorXd geo = heat_data.heat_solver.solve(rhs);

      double minDist = 1e9;
      for (int idx : group2)
        minDist = std::min(minDist, geo(idx));

      // If geodesic distance is low, merge
      if (minDist < 0.05)  // threshold: tweak as needed
      {
        for (int idx : group2)
          assignments[idx] = b1;
      }
    }
  }
}

/**
 * @brief Merges vertices within a 3D Euclidean distance (L2) of each other.
 * Unlike grid-based (L1) welders, this ensures diagonal gaps are bridged.
 *
 * @param V Initial Nx3 vertex matrix (raw stitched chunks).
 * @param F Initial Mx3 face matrix (global soup).
 * @param epsilon The max distance between two vertices to be considered "identical".
 * @param SV [Output] Unique vertex positions (the "Survivors").
 * @param SVI [Output] Map: Unique Index -> Original Index (first representative).
 * @param SVJ [Output] Map: Original Index -> Unique Index (the "Zipper" map).
 * @param SF [Output] Updated face indices pointing to the new unique vertices.
 */
void weld_mesh_euclidean(const Eigen::MatrixXd& V,  // Input: N x 3 (original vertices)
                         const Eigen::MatrixXi& F,  // Input: M x 3 (original faces)
                         double epsilon,            // Input: Threshold (e.g., 0.001)
                         Eigen::MatrixXd& SV,       // Output: Unique Vertices
                         Eigen::VectorXi& SVI,      // Output: Surviving Indices
                         Eigen::VectorXi& SVJ,      // Output: Mapping (Old -> New)
                         Eigen::MatrixXi& SF)       // Output: Updated Faces
{
  const int N = static_cast<int>(V.rows());
  SVJ.setConstant(N, -1);
  std::vector<int> survivors;
  std::vector<Eigen::Vector3d> unique_list;

  // 1. Identify Unique Vertices based on actual L2 distance
  for (int i = 0; i < N; ++i)
  {
    Eigen::Vector3d p = V.row(i);
    int match = -1;

    // Linear search is fine for 2k-10k vertices.
    // If it's too slow, we can add a spatial hash later.
    for (int j = 0; j < (int)unique_list.size(); ++j)
    {
      if ((p - unique_list[j]).norm() <= epsilon)
      {
        match = j;
        break;
      }
    }

    if (match == -1)
    {
      SVJ(i) = static_cast<int>(unique_list.size());
      unique_list.push_back(p);
      survivors.push_back(i);
    }
    else
    {
      SVJ(i) = match;
    }
  }

  // 2. Build SV (Unique Vertices)
  SV.resize(unique_list.size(), 3);
  for (int i = 0; i < (int)unique_list.size(); ++i)
  {
    SV.row(i) = unique_list[i];
  }

  // 3. Build SVI (The first index that represented this unique spot)
  SVI.resize(survivors.size());
  for (int i = 0; i < (int)survivors.size(); ++i)
  {
    SVI(i) = survivors[i];
  }

  // 4. Build SF (The "Zippered" Faces)
  SF.resize(F.rows(), 3);
  for (int i = 0; i < F.rows(); ++i)
  {
    for (int j = 0; j < 3; ++j)
    {
      SF(i, j) = SVJ(F(i, j));
    }
  }
}

Common::Vec3 ReadPosition(const u8* vert_ptr, u32 position_offset)
{
  Common::Vec3 vertex_position;
  std::memcpy(&vertex_position.x, vert_ptr + position_offset, sizeof(float));
  std::memcpy(&vertex_position.y, vert_ptr + sizeof(float) + position_offset, sizeof(float));
  std::memcpy(&vertex_position.z, vert_ptr + sizeof(float) * 2 + position_offset, sizeof(float));
  return vertex_position;
}

std::vector<Common::Vec3> ReadMeshPositions(const GraphicsModSystem::DrawDataView& draw_data)
{
  std::vector<Common::Vec3> result;
  for (u32 i = 0; i < draw_data.vertex_count; i++)
  {
    const auto pos =
        ReadPosition(draw_data.vertex_data + i * draw_data.vertex_format->GetVertexStride(),
                     draw_data.vertex_format->GetVertexDeclaration().position.offset);
    result.push_back(pos);
  }
  return result;
}

Eigen::Matrix3Xd ConvertMeshPositions(const std::vector<Common::Vec3>& positions,
                                      const GraphicsModSystem::DrawDataView& draw_data)
{
  const auto& declaration = draw_data.vertex_format->GetVertexDeclaration();

  Common::Matrix44 position_transform;
  if (!declaration.posmtx.enable)
  {
    Common::Matrix44 object_transform;
    for (std::size_t i = 0; i < 3; i++)
    {
      object_transform.data[i * 4 + 0] = draw_data.object_transform[i][0];
      object_transform.data[i * 4 + 1] = draw_data.object_transform[i][1];
      object_transform.data[i * 4 + 2] = draw_data.object_transform[i][2];
      object_transform.data[i * 4 + 3] = draw_data.object_transform[i][3];
    }
    object_transform.data[12] = 0;
    object_transform.data[13] = 0;
    object_transform.data[14] = 0;
    object_transform.data[15] = 1;
    position_transform = object_transform;
  }

  Eigen::Matrix3Xd result(3, positions.size());
  for (std::size_t i = 0; i < positions.size(); ++i)
  {
    if (declaration.posmtx.enable)
    {
      // Position transform is actually vertex specific
      u32 gpu_skin_index;
      std::memcpy(&gpu_skin_index,
                  draw_data.vertex_data + i * declaration.stride + declaration.posmtx.offset,
                  sizeof(u32));

      for (std::size_t j = 0; j < 3; j++)
      {
        position_transform.data[j * 4 + 0] =
            draw_data.gpu_skinning_position_transform[gpu_skin_index + j][0];
        position_transform.data[j * 4 + 1] =
            draw_data.gpu_skinning_position_transform[gpu_skin_index + j][1];
        position_transform.data[j * 4 + 2] =
            draw_data.gpu_skinning_position_transform[gpu_skin_index + j][2];
        position_transform.data[j * 4 + 3] =
            draw_data.gpu_skinning_position_transform[gpu_skin_index + j][3];
      }
      position_transform.data[12] = 0;
      position_transform.data[13] = 0;
      position_transform.data[14] = 0;
      position_transform.data[15] = 1;
    }
    const auto pos = positions[i];
    const auto pos_object_space = position_transform.Transform(pos, 1);
    result.col(i) = Eigen::Vector3d(pos_object_space.x, pos_object_space.y, pos_object_space.z);
  }

  return result;
}

bool IsDuplicateMesh(const Eigen::Matrix3Xd& mesh,
                     const std::vector<Eigen::Matrix3Xd>& existing_meshes, float epsilon = 1e-4f)
{
  for (const auto& existing_mesh : existing_meshes)
  {
    Eigen::Matrix3Xd diff = (mesh - existing_mesh).cwiseAbs();

    if (diff.maxCoeff() < epsilon)
    {
      return true;
    }
  }
  return false;
}

/**
 * @brief Combines individual 3xN chunks into one 3xTotal global matrix.
 * @param chunks A vector of matrices, one for each capture chunk.
 * @return A single Eigen::Matrix3Xd representing the full captured pose.
 */
Eigen::Matrix3Xd StitchChunks(const std::vector<Eigen::Matrix3Xd>& chunks)
{
  // 1. Calculate total vertex count
  int total_vertices = 0;
  for (const auto& chunk : chunks)
  {
    total_vertices += (int)chunk.cols();
  }

  // 2. Pre-allocate the global matrix [3 rows x Total Cols]
  Eigen::Matrix3Xd V_global(3, total_vertices);

  // 3. Fill the global matrix chunk-by-chunk
  int current_offset = 0;
  for (const auto& chunk : chunks)
  {
    // block(start_row, start_col, num_rows, num_cols)
    V_global.block(0, current_offset, 3, chunk.cols()) = chunk;
    current_offset += (int)chunk.cols();
  }

  return V_global;
}

double FindMinimumChunkGap(const Eigen::Matrix3Xd& restPose,
                           const std::vector<GraphicsModSystem::DrawCallID>& vertex_per_draw_call)
{
  const int N = static_cast<int>(restPose.cols());
  double minTrueGap = 1e9;
  bool foundAnyGap = false;

  for (int i = 0; i < N; ++i)
  {
    for (int j = i + 1; j < N; ++j)
    {
      if (vertex_per_draw_call[i] != vertex_per_draw_call[j])
      {
        double d = (restPose.col(i) - restPose.col(j)).norm();

        // IGNORE exact overlaps (already handled or noise)
        // We want the smallest REAL distance between separate vertices
        if (d > 1e-7 && d < minTrueGap)
        {
          minTrueGap = d;
          foundAnyGap = true;
        }
      }
    }
  }

  // If we found a real gap (like your 0.00023),
  // add a 20% buffer to ensure the weld catches it.
  if (foundAnyGap)
    return minTrueGap * 1.2;

  // Fallback: If everything was 0, use a standard safe epsilon
  return 1e-4;
}

std::vector<Eigen::Matrix3Xd> ReassembleSignificantFrames(
    const std::map<GraphicsModSystem::DrawCallID, GraphicsModEditor::VertexGroupDumper::FinalData>&
        draw_call_to_mesh_data)
{
  std::set<int> frames_with_changes;
  std::vector<Eigen::Matrix3Xd> lastKnownPoses;
  for (const auto& [draw_call, data] : draw_call_to_mesh_data)
  {
    for (auto const& [frame_id, mesh_index] : data.frame_id_to_mesh_index)
    {
      frames_with_changes.insert(frame_id);
    }

    // Initialize with our rest pose (first frame)
    lastKnownPoses.push_back(data.mesh_poses[0]);
  }

  std::vector<Eigen::Matrix3Xd> motionFrames;
  for (int frame_id : frames_with_changes)
  {
    std::size_t draw_call_count = 0;
    for (const auto& [draw_call, data] : draw_call_to_mesh_data)
    {
      if (auto it = data.frame_id_to_mesh_index.find(frame_id);
          it != data.frame_id_to_mesh_index.end())
      {
        lastKnownPoses[draw_call_count] = data.mesh_poses[it->second];
      }
      draw_call_count++;
    }
    // This frame now contains the most recent data for every chunk
    motionFrames.push_back(StitchChunks(lastKnownPoses));
  }

  return motionFrames;
}

/**
 * Normalizes an animation sequence after it has been welded.
 * 1. Centers Pose 0 at the origin.
 * 2. Scales based on Pose 0 bounding box.
 * 3. Aligns all frames to Pose 0.
 */
void NormalizeWeldedAnimation(std::vector<Eigen::Matrix3Xd>& poses, double& outScale,
                              Eigen::Vector3d& centroid)
{
  if (poses.empty())
    return;

  // A. Center and Scale based on Pose 0
  centroid = poses[0].rowwise().mean();
  for (auto& p : poses)
    p.colwise() -= centroid;

  Eigen::Vector3d minB = poses[0].rowwise().minCoeff();
  Eigen::Vector3d maxB = poses[0].rowwise().maxCoeff();
  outScale = (maxB - minB).norm();

  if (outScale > 1e-8)
  {
    for (auto& p : poses)
      p /= outScale;
  }

  // B. Global Alignment
  // We align everything to poses[0] (which is now centered/scaled)
  const Eigen::Matrix3Xd& reference = poses[0];

  for (size_t k = 1; k < poses.size(); ++k)
  {
    Eigen::Matrix3Xd& currentPose = poses[k];

    // 1. Center the current frame locally
    Eigen::Vector3d currentCentroid = currentPose.rowwise().mean();
    Eigen::Matrix3Xd centeredCurrent = currentPose.colwise() - currentCentroid;

    // 2. Solve for Rotation (Kabsch)
    // Note: Reference is already at origin, so we use reference directly
    Eigen::Matrix3d H = centeredCurrent * reference.transpose();

    Eigen::JacobiSVD<Eigen::Matrix3d> svd(H, Eigen::ComputeFullU | Eigen::ComputeFullV);
    Eigen::Matrix3d R = svd.matrixV() * svd.matrixU().transpose();

    if (R.determinant() < 0)
    {
      Eigen::Matrix3d V = svd.matrixV();
      V.col(2) *= -1;
      R = V * svd.matrixU().transpose();
    }

    // 3. Update the pose: Remove its translation, rotate it,
    // and keep it at the origin to match reference.
    currentPose = R * centeredCurrent;
  }
}

/**
 * Solves the "Procrustes Problem" (Kabsch Algorithm) for a set of points.
 *
 * Given a group of vertices in their 'Rest Pose' and 'Animated Pose',
 * this function finds the single best Rotation (R) and Translation (T)
 * that aligns the rest points to the animated ones.
 *
 * It calculates the 'Sum of Squared Errors' (SSE) in World Space.
 * - If SSE is near 0: The group moves as a perfect rigid bone.
 * - If SSE is high: The group is stretching or contains multiple independent motions.
 *
 * @return The total error (SSE) for the best possible rigid fit of this bone.
 */
double CalculateRigidSSE_WorldSpace(const std::vector<int>& indices,
                                    const std::vector<Eigen::Matrix3Xd>& allPoses,
                                    const Eigen::Matrix3Xd& restPose)
{
  if (indices.empty())
    return 0.0;
  const std::size_t N = indices.size();

  // 1. Reference Subset (3 x N)
  Eigen::Matrix3Xd P(3, N);
  for (std::size_t i = 0; i < N; ++i)
    P.col(i) = restPose.col(indices[i]);
  Eigen::Vector3d p_mean = P.rowwise().mean();
  Eigen::Matrix3Xd P_centered = P.colwise() - p_mean;

  double totalSSE = 0.0;

  for (const auto& pose : allPoses)
  {
    // 2. Target Subset (3 x N)
    Eigen::Matrix3Xd Q(3, N);
    for (std::size_t i = 0; i < N; ++i)
      Q.col(i) = pose.col(indices[i]);
    Eigen::Vector3d q_mean = Q.rowwise().mean();
    Eigen::Matrix3Xd Q_centered = Q.colwise() - q_mean;

    // 3. Kabsch Rotation
    Eigen::Matrix3d H = P_centered * Q_centered.transpose();  // Note: P*Q' for ref->pose
    Eigen::JacobiSVD<Eigen::Matrix3d> svd(H, Eigen::ComputeFullU | Eigen::ComputeFullV);

    Eigen::Matrix3d V = svd.matrixV();
    Eigen::Matrix3d U = svd.matrixU();
    Eigen::Matrix3d R = V * U.transpose();

    if (R.determinant() < 0)
    {
      V.col(2) *= -1;
      R = V * U.transpose();
    }

    // 4. Full Translation
    Eigen::Vector3d T = q_mean - R * p_mean;

    // 5. World Space Error (The Ultimate Truth)
    for (std::size_t i = 0; i < N; ++i)
    {
      Eigen::Vector3d reconstructed = R * P.col(i) + T;
      totalSSE += (reconstructed - Q.col(i)).squaredNorm();
    }
  }
  return totalSSE;
}

/**
 * Performs a binary split of a vertex group based on raw motion data.
 *
 * Instead of looking at 3D shapes, this function treats each vertex as a
 * point in a high-dimensional "Motion Space" (3 * Number of Frames).
 * It uses a stable 2-seed K-Means clustering to find the two most
 * distinct motion patterns within the group.
 *
 * @param allPoses The complete animation data.
 * @param targetIndices The specific subset of vertices to be split.
 * @param assignments [Out] Maps vertex indices to Group 0 or Group 1.
 */
void rigid_split_raw(const std::vector<Eigen::Matrix3Xd>& allPoses,
                     const std::vector<int>& targetIndices, std::vector<int>& assignments)
{
  const std::size_t N_sub = targetIndices.size();
  const std::size_t K = allPoses.size();

  // 1. Build a local Motion Descriptor (Vertices x 3K coordinates)
  Eigen::MatrixXd motion(N_sub, 3 * K);
  for (std::size_t i = 0; i < N_sub; ++i)
  {
    int vIdx = targetIndices[i];
    for (std::size_t k = 0; k < K; ++k)
    {
      motion.row(i).segment<3>(k * 3) = allPoses[k].col(vIdx);
    }
  }

  // 2. Farthest-First Traversal for K=2 Seeds (Stable Initialization)
  int s1 = 0;
  int s2 = -1;
  double maxD = -1;
  for (std::size_t i = 1; i < N_sub; ++i)
  {
    double d = (motion.row(i) - motion.row(s1)).squaredNorm();
    if (d > maxD)
    {
      maxD = d;
      s2 = static_cast<int>(i);
    }
  }

  // 3. Iterative Lloyd's K-Means
  //
  // Use RowVectorXd to match the shape of motion.row(i)
  Eigen::RowVectorXd c1 = motion.row(s1);
  Eigen::RowVectorXd c2 = motion.row(s2);

  for (int iter = 0; iter < 15; ++iter)
  {
    std::vector<int> g1_local, g2_local;

    // Assignment
    for (std::size_t i = 0; i < N_sub; ++i)
    {
      double d1 = (motion.row(i) - c1).squaredNorm();
      double d2 = (motion.row(i) - c2).squaredNorm();
      int best_k = (d1 < d2) ? 0 : 1;
      assignments[targetIndices[i]] = best_k;

      if (best_k == 0)
        g1_local.push_back(i);
      else
        g2_local.push_back(i);
    }

    if (g1_local.empty() || g2_local.empty())
      break;

    // Update
    c1.setZero();
    for (int idx : g1_local)
      c1 += motion.row(idx);
    c1 /= static_cast<double>(g1_local.size());

    c2.setZero();
    for (int idx : g2_local)
      c2 += motion.row(idx);
    c2 /= static_cast<double>(g2_local.size());
  }
}

double CalculateSingleVertexSSE(int vIdx, const std::vector<Eigen::Matrix3Xd>& allPoses,
                                const Eigen::Matrix3Xd& restPose)
{
  double sse = 0.0;
  Eigen::Vector3d restPos = restPose.col(vIdx);

  for (const auto& pose : allPoses)
  {
    // We measure how far the vertex traveled from its rest position
    sse += (pose.col(vIdx) - restPos).squaredNorm();
  }
  return sse;
}

double FindNarrowestSplitAxis(const std::vector<int>& indices, const Eigen::Matrix3Xd& restPose)
{
  Eigen::Vector3d minB(1e9, 1e9, 1e9), maxB(-1e9, -1e9, -1e9);
  for (int idx : indices)
  {
    Eigen::Vector3d p = restPose.col(idx);
    minB = minB.cwiseMin(p);
    maxB = maxB.cwiseMax(p);
  }
  Eigen::Vector3d span = maxB - minB;
  if (span.x() < span.y() && span.x() < span.z())
    return 0;
  if (span.y() < span.x() && span.y() < span.z())
    return 1;
  return 2;
}

// --- Add frame importance calculation helper ---
double ComputeFrameImportance(const Eigen::Matrix3Xd& pose, const Eigen::Matrix3Xd& restPose)
{
  // Use variance of motion as importance
  double total = 0.0;
  for (int i = 0; i < pose.cols(); ++i)
    total += (pose.col(i) - restPose.col(i)).squaredNorm();
  return std::sqrt(total / pose.cols());
}

// --- Hybrid split function: motion + geometry ---
void rigid_split_raw_hybrid(const std::vector<Eigen::Matrix3Xd>& allPoses,
                            const std::vector<int>& targetIndices, std::vector<int>& assignments,
                            const Eigen::Matrix3Xd& restPose)
{
  // Weighted motion descriptor
  const std::size_t N_sub = targetIndices.size();
  const std::size_t K = allPoses.size();
  Eigen::MatrixXd motion(N_sub, 3 * K);
  std::vector<double> frame_weights(K);
  for (std::size_t k = 0; k < K; ++k)
    frame_weights[k] = ComputeFrameImportance(allPoses[k], restPose);

  for (std::size_t i = 0; i < N_sub; ++i)
  {
    int vIdx = targetIndices[i];
    for (std::size_t k = 0; k < K; ++k)
      motion.row(i).segment<3>(k * 3) = frame_weights[k] * allPoses[k].col(vIdx);
  }

  // Standard motion clustering
  int s1 = 0;
  int s2 = -1;
  double maxD = -1;
  for (std::size_t i = 1; i < N_sub; ++i)
  {
    double d = (motion.row(i) - motion.row(s1)).squaredNorm();
    if (d > maxD)
    {
      maxD = d;
      s2 = static_cast<int>(i);
    }
  }
  Eigen::RowVectorXd c1 = motion.row(s1);
  Eigen::RowVectorXd c2 = motion.row(s2);

  for (int iter = 0; iter < 15; ++iter)
  {
    std::vector<int> g1_local, g2_local;
    for (std::size_t i = 0; i < N_sub; ++i)
    {
      double d1 = (motion.row(i) - c1).squaredNorm();
      double d2 = (motion.row(i) - c2).squaredNorm();
      int best_k = (d1 < d2) ? 0 : 1;
      assignments[targetIndices[i]] = best_k;
      if (best_k == 0)
        g1_local.push_back(i);
      else
        g2_local.push_back(i);
    }
    if (g1_local.empty() || g2_local.empty())
      break;
    c1.setZero();
    for (int idx : g1_local)
      c1 += motion.row(idx);
    c1 /= static_cast<double>(g1_local.size());
    c2.setZero();
    for (int idx : g2_local)
      c2 += motion.row(idx);
    c2 /= static_cast<double>(g2_local.size());
  }

  // Geometric bottleneck override
  int axis = FindNarrowestSplitAxis(targetIndices, restPose);
  double minA = 1e9, maxA = -1e9;
  for (int idx : targetIndices)
  {
    double val = restPose(axis, idx);
    minA = std::min(minA, val);
    maxA = std::max(maxA, val);
  }
  double midA = (minA + maxA) / 2.0;
  // If group is very narrow, force split at midpoint
  if ((maxA - minA) < 0.15)  // threshold: tweak as needed
  {
    for (int idx : targetIndices)
    {
      double val = restPose(axis, idx);
      assignments[idx] = (val < midA) ? 0 : 1;
    }
  }
}

void rigid_split_raw_geodesic(const std::vector<Eigen::Matrix3Xd>& allPoses,
                              const std::vector<int>& targetIndices, std::vector<int>& assignments,
                              const SimpleGeodesic::HeatGeodesicsData& heat_data,
                              bool force_topological)
{
  const std::size_t N_sub = targetIndices.size();
  const std::size_t K = allPoses.size();

  // 1. Build Motion Space (Used for K-Means refinement)
  Eigen::MatrixXd motion(N_sub, 3 * K);
  for (std::size_t i = 0; i < N_sub; ++i)
  {
    int vIdx = targetIndices[i];
    for (std::size_t k = 0; k < K; ++k)
    {
      motion.row(i).segment<3>(k * 3) = allPoses[k].col(vIdx);
    }
  }

  int s1 = 0, s2 = 0;

  // 2. SEED SELECTION (The "Anatomical Foundation")
  if (force_topological)
  {
    // Determine the longest axis of the CURRENT group (0=X, 1=Y, 2=Z)
    Eigen::Vector3d minB(1e9, 1e9, 1e9), maxB(-1e9, -1e9, -1e9);
    for (int idx : targetIndices)
    {
      Eigen::Vector3d p = heat_data.V.row(idx).transpose();
      minB = minB.cwiseMin(p);
      maxB = maxB.cwiseMax(p);
    }

    Eigen::Vector3d span = maxB - minB;
    int axis = 0;
    if (span.y() > span.x() && span.y() > span.z())
      axis = 1;
    else if (span.z() > span.x() && span.z() > span.y())
      axis = 2;

    // Force seeds to the extreme ends of the LONGEST axis
    double minVal = 1e9, maxVal = -1e9;
    for (int i = 0; i < (int)N_sub; ++i)
    {
      double val = heat_data.V(targetIndices[i], axis);
      if (val < minVal)
      {
        minVal = val;
        s1 = i;
      }
      if (val > maxVal)
      {
        maxVal = val;
        s2 = i;
      }
    }
  }
  else
  {
    // Standard "Active" Seed 1
    double maxSSE = -1.0;
    for (std::size_t i = 0; i < N_sub; ++i)
    {
      double sse = CalculateSingleVertexSSE(targetIndices[i], allPoses, allPoses[0]);
      if (sse > maxSSE)
      {
        maxSSE = sse;
        s1 = (int)i;
      }
    }
    // Seed 2: Furthest Geodesic
    Eigen::VectorXd rhs1 = Eigen::VectorXd::Zero(heat_data.V.rows());
    rhs1(targetIndices[s1]) = 1.0;
    Eigen::VectorXd geo1 = heat_data.heat_solver.solve(rhs1);

    // Normalize geo1 to [0,1]
    {
      double minv = geo1.minCoeff();
      geo1.array() -= minv;
      double maxv = geo1.maxCoeff();
      if (maxv > 1e-12)
        geo1 /= maxv;
    }

    double minHeat = 2.0;
    for (std::size_t i = 0; i < N_sub; ++i)
    {
      if (geo1(targetIndices[i]) < minHeat)
      {
        minHeat = geo1(targetIndices[i]);
        s2 = (int)i;
      }
    }
  }

  // 3. HYBRID K-MEANS WITH "SPATIAL LOCK"
  Eigen::VectorXd rhs1 = Eigen::VectorXd::Zero(heat_data.V.rows());
  rhs1(targetIndices[s1]) = 1.0;
  Eigen::VectorXd geo1 = heat_data.heat_solver.solve(rhs1);

  Eigen::VectorXd rhs2 = Eigen::VectorXd::Zero(heat_data.V.rows());
  rhs2(targetIndices[s2]) = 1.0;
  Eigen::VectorXd geo2 = heat_data.heat_solver.solve(rhs2);

  // Normalize geodesic fields to [0,1] to keep them comparable to motion costs
  {
    double min1 = geo1.minCoeff();
    geo1.array() -= min1;
    double max1 = geo1.maxCoeff();
    if (max1 > 1e-12)
      geo1 /= max1;

    double min2 = geo2.minCoeff();
    geo2.array() -= min2;
    double max2 = geo2.maxCoeff();
    if (max2 > 1e-12)
      geo2 /= max2;
  }

  Eigen::RowVectorXd c1 = motion.row(s1);
  Eigen::RowVectorXd c2 = motion.row(s2);

  for (int iter = 0; iter < 15; ++iter)
  {
    std::vector<int> g1_local, g2_local;
    for (std::size_t i = 0; i < N_sub; ++i)
    {
      int gIdx = targetIndices[i];

      // If forcing topological, ignore motion for first 5 iters to LOCK the cut
      if (force_topological && iter < 5)
      {
        assignments[gIdx] = (geo1(gIdx) > geo2(gIdx)) ? 0 : 1;
      }
      else
      {
        double d1_m = (motion.row(i) - c1).squaredNorm();
        double d2_m = (motion.row(i) - c2).squaredNorm();

        // Physical Euclidean distance helps prevent "Heat Jumps"
        double d1_phys = (heat_data.V.row(gIdx) - heat_data.V.row(targetIndices[s1])).squaredNorm();
        double d2_phys = (heat_data.V.row(gIdx) - heat_data.V.row(targetIndices[s2])).squaredNorm();

        // Use both Geodesic and Physical distance to decide the "Cut"
        double cost1 = d1_m * (geo1(gIdx) + d1_phys * 0.1);
        double cost2 = d2_m * (geo2(gIdx) + d2_phys * 0.1);

        assignments[gIdx] = (cost1 < cost2) ? 0 : 1;
      }
      if (assignments[gIdx] == 0)
        g1_local.push_back((int)i);
      else
        g2_local.push_back((int)i);
    }
    if (g1_local.empty() || g2_local.empty())
      break;
    c1.setZero();
    for (int idx : g1_local)
      c1 += motion.row(idx);
    c1 /= g1_local.size();
    c2.setZero();
    for (int idx : g2_local)
      c2 += motion.row(idx);
    c2 /= g2_local.size();
  }
}

double CalculateGroupGeodesicDiameter(const std::vector<int>& indices,
                                      const SimpleGeodesic::HeatGeodesicsData& heat_data)
{
  if (indices.size() < 10)
    return 0.0;

  Eigen::VectorXd rhs = Eigen::VectorXd::Zero(heat_data.V.rows());
  for (int idx : indices)
    rhs(idx) = 1.0;

  Eigen::VectorXd geo = heat_data.heat_solver.solve(rhs);

  // Do NOT normalize geo globally to [0,1]
  double max_h = -1e9;
  double min_h = 1e9;
  for (int idx : indices)
  {
    double h = geo(idx);
    if (h > max_h)
      max_h = h;
    if (h < min_h)
      min_h = h;
  }
  return std::max(0.0, max_h - min_h);
}

bool AreGroupsPhysicallyAdjacent(const std::vector<int>& g1, const std::vector<int>& g2,
                                 const Eigen::MatrixXi& F)
{
  std::set<int> g1_set(g1.begin(), g1.end());
  std::set<int> g2_set(g2.begin(), g2.end());
  for (int i = 0; i < F.rows(); ++i)
  {
    int a = F(i, 0), b = F(i, 1), c = F(i, 2);
    if ((g1_set.count(a) && g2_set.count(b)) || (g1_set.count(b) && g2_set.count(a)) ||
        (g1_set.count(a) && g2_set.count(c)) || (g1_set.count(c) && g2_set.count(a)) ||
        (g1_set.count(b) && g2_set.count(c)) || (g1_set.count(c) && g2_set.count(b)))
      return true;
  }
  return false;
}

/**
 * Orchestrates the recursive bone discovery process using an SSE-based "Gate".
 *
 * This is the "Brain" of the bone detection. It identifies which vertex groups
 * are the most 'rubbery' (high World-Space SSE) and uses 'rigid_split_raw' to
 * break them down. It includes safeguard 'Gates' to prevent the creation of
 * microscopic bones or bones that don't significantly improve the animation's accuracy.
 *
 * @param tolerance The SSE threshold that triggers a split.
 * @return A flat array of Bone IDs for every vertex in the mesh.
 */
std::vector<int> CalculateVertexGroupsAdaptive(const std::vector<Eigen::Matrix3Xd>& allPoses,
                                               const Eigen::Matrix3Xd& restPose, double tolerance)
{
  const int numVertices = restPose.cols();
  std::vector<int> allIndices(numVertices);
  std::iota(allIndices.begin(), allIndices.end(), 0);

  struct BoneGroup
  {
    std::vector<int> indices;
    bool skip_split = false;
  };

  std::vector<BoneGroup> groups = {{allIndices, false}};
  const int MAX_BONES = 50;

  const double totalInitialSSE = CalculateRigidSSE_WorldSpace(allIndices, allPoses, restPose);

  while (groups.size() < MAX_BONES)
  {
    int split_index = -1;
    double maxSSE = -1;

    // Find worst group that isn't already "done"
    for (std::size_t i = 0; i < groups.size(); ++i)
    {
      if (groups[i].skip_split)
        continue;
      double sse = CalculateRigidSSE_WorldSpace(groups[i].indices, allPoses, restPose);
      if (sse > tolerance && sse > maxSSE)
      {
        maxSSE = sse;
        split_index = static_cast<int>(i);
      }
    }

    if (split_index == -1)
      break;

    // Perform the RAW motion split
    std::vector<int> sub_assignments(numVertices, -1);
    rigid_split_raw_hybrid(allPoses, groups[split_index].indices, sub_assignments, restPose);

    std::vector<int> g1, g2;
    for (int vIdx : groups[split_index].indices)
    {
      if (sub_assignments[vIdx] == 0)
        g1.push_back(vIdx);
      else
        g2.push_back(vIdx);
    }

    if (g1.empty() || g2.empty())
    {
      groups[split_index].skip_split = true;
      continue;
    }

    // IMPROVEMENT GATE (Using SSE for mathematical fairness)
    // Calculate total error
    const double parentSSE =
        CalculateRigidSSE_WorldSpace(groups[split_index].indices, allPoses, restPose);
    const double child1SSE = CalculateRigidSSE_WorldSpace(g1, allPoses, restPose);
    const double child2SSE = CalculateRigidSSE_WorldSpace(g2, allPoses, restPose);
    const double combinedChildSSE = child1SSE + child2SSE;

    double improvementFactor = combinedChildSSE / parentSSE;

    WARN_LOG_FMT(
        VIDEO,
        "Step: {} | Parent size: {} | Child 1 size: {} | Child 2 size: {}, Improvement Factor: {}, "
        "Active "
        "groups: {}, Parent SSE: {} | Child 1 SSE: {} | Child 2 SSE: {} | Combined SSE: {}",
        groups.size(), groups[split_index].indices.size(), g1.size(), g2.size(), improvementFactor,
        std::count_if(groups.begin(), groups.end(), [](auto& g) { return !g.skip_split; }),
        parentSSE, child1SSE, child2SSE, combinedChildSSE);

    const double improvement = parentSSE - combinedChildSSE;

    // GATE A: The "Significant Contribution" Check
    // If the improvement is less than 0.01% of the total mesh error, it's not worth a new bone.
    if (improvement < (totalInitialSSE * 0.0001))
    {
      WARN_LOG_FMT(VIDEO,
                   "Skipping parent group due to no significant contribution, actual improvement "
                   "{}, totalInitialSSE {}, minimum improvement required {}",
                   improvement, totalInitialSSE, totalInitialSSE * 0.001);
      groups[split_index].skip_split = true;
      continue;
    }

    // GATE B: The "Absolute Size" Check
    // Don't create bones smaller than 1% of the total mesh (e.g., 25 vertices for a 2500 mesh)
    if (g1.size() < (numVertices * 0.01) || g2.size() < (numVertices * 0.01))
    {
      WARN_LOG_FMT(VIDEO, "Skipping parent group due to small size, max vertices allowed {}",
                   numVertices * 0.01);
      groups[split_index].skip_split = true;
      continue;
    }

    // Commit the split
    groups.erase(groups.begin() + split_index);
    groups.emplace_back(g1, false);
    groups.emplace_back(g2, false);
  }

  // Map back to final array
  std::vector<int> finalAssignments(numVertices, 0);
  for (std::size_t g = 0; g < groups.size(); ++g)
  {
    for (int vIdx : groups[g].indices)
      finalAssignments[vIdx] = static_cast<int>(g);
  }
  return finalAssignments;
}

/**
 * @brief Converts 'Hard' bone assignments into 'Smooth' Linear Blend Skinning (LBS) weights.
 *
 * Uses the Heat Equation with a "Sovereign Shield" to protect small anatomical
 * bones (like the Neck or Hands) from being drowned out by the massive Spine/Core.
 */
Eigen::MatrixXd ComputeHarmonicWeights(const std::vector<int>& assignments,
                                       const Eigen::Matrix3Xd& V, const Eigen::MatrixXi& F,
                                       const std::vector<Eigen::Matrix3Xd>& allPoses,
                                       const SimpleGeodesic::HeatGeodesicsData& heat_data,
                                       int numBones)
{
  const int N = static_cast<int>(V.cols());
  Eigen::MatrixXd W = Eigen::MatrixXd::Zero(N, numBones);

  std::vector<int> counts(numBones, 0);
  for (int a : assignments)
  {
    if (a >= 0 && a < numBones)
      counts[a]++;
  }

  // 1. ROBUST CORE DETECTION (Geometry-Agnostic)
  int core_idx = 0;
  double min_avg_sse = 1e12;

  for (int b = 0; b < numBones; ++b)
  {
    double total_sse = 0;
    int count = 0;
    for (int i = 0; i < N; ++i)
    {
      if (assignments[i] == b)
      {
        total_sse += CalculateSingleVertexSSE(i, allPoses, V);
        count++;
      }
    }
    if (count > 0)
    {
      double avg_sse = total_sse / (double)count;
      if (avg_sse < min_avg_sse)
      {
        min_avg_sse = avg_sse;
        core_idx = b;
      }
    }
  }

  // 2. COMPUTE RAW WEIGHTS (Diffusion)
  for (int b = 0; b < numBones; ++b)
  {
    Eigen::VectorXd delta = Eigen::VectorXd::Zero(N);
    bool has_verts = false;
    for (int i = 0; i < N; ++i)
    {
      if (assignments[i] == b)
      {
        delta(i) = 1.0;
        has_verts = true;
      }
    }
    if (!has_verts)
      continue;

    Eigen::VectorXd proximity = heat_data.heat_solver.solve(delta);

    // Normalize proximity to avoid scale blow-up
    {
      double maxp = proximity.maxCoeff();
      if (maxp > 1e-12)
        proximity /= maxp;
    }

    for (int i = 0; i < N; ++i)
    {
      // Power 1.5: Create a smooth "S-Curve" falloff instead of a "Cliff"
      W(i, b) = std::pow(std::max(0.0, proximity(i)), 1.5);
    }
  }

  // 3. INITIAL NORMALIZATION
  for (int i = 0; i < N; ++i)
  {
    double s = W.row(i).sum();
    if (s > 1e-9)
      W.row(i) /= s;
    else
      W(i, assignments[i]) = 1.0;
  }

  // 4. LAPLACIAN SMOOTHING PASS (The "Patchwork" Eraser)
  std::vector<std::vector<int>> adj(N);
  for (int i = 0; i < F.rows(); ++i)
  {
    for (int j = 0; j < 3; ++j)
    {
      int u = F(i, j), v = F(i, (j + 1) % 3);
      adj[u].push_back(v);
      adj[v].push_back(u);
    }
  }

  for (int iter = 0; iter < 2; ++iter)
  {
    Eigen::MatrixXd W_smooth = W;
    for (int i = 0; i < N; ++i)
    {
      if (adj[i].empty())
        continue;
      Eigen::RowVectorXd sum = W.row(i);
      for (int neighbor : adj[i])
        sum += W.row(neighbor);
      W_smooth.row(i) = sum / (double)(adj[i].size() + 1);
    }
    W = W_smooth;
  }

  // 5. FINAL RE-NORMALIZATION
  for (int i = 0; i < N; ++i)
  {
    double s = W.row(i).sum();
    if (s > 1e-9)
      W.row(i) /= s;
  }

  for (int b = 0; b < numBones; ++b)
  {
    if (counts[b] > 0)
    {
      // Find center of this bone
      Eigen::Vector3d avgP = Eigen::Vector3d::Zero();
      int bCount = 0;
      for (int i = 0; i < N; ++i)
      {
        if (assignments[i] == b)
        {
          avgP += V.col(i);
          bCount++;
        }
      }
      avgP /= bCount;
      WARN_LOG_FMT(VIDEO, "Bone {}: {} verts, Pos: ({:.2f}, {:.2f}, {:.2f}) {}", b, bCount,
                   avgP.x(), avgP.y(), avgP.z(), (b == core_idx ? "[CORE]" : ""));
    }
  }

  WARN_LOG_FMT(VIDEO, "Core Bone identified as: {} (Min SSE: {:.6f})", core_idx, min_avg_sse);
  return W;
}

/**
 * @brief Computes a centroid weighted by "Rigidity".
 *
 * High-stretch vertices (neck, armpit) are given low weight.
 * Rigid vertices (skull, mid-limb) are given high weight.
 * Result: The pivot naturally moves toward the "bone" and away from the "skin".
 */
Eigen::Vector3d ComputeRobustCentroid(const std::vector<int>& indices,
                                      const std::vector<Eigen::Matrix3Xd>& allPoses,
                                      const Eigen::Matrix3Xd& restPose)
{
  if (indices.empty())
    return Eigen::Vector3d::Zero();

  const size_t N = indices.size();

  // 1. Manually build the Rest Pose matrix for this group
  Eigen::Matrix3Xd P(3, N);
  for (size_t i = 0; i < N; ++i)
  {
    P.col(i) = restPose.col(indices[i]);
  }

  const Eigen::Vector3d p_mean = P.rowwise().mean();
  const Eigen::Matrix3Xd P_centered = P.colwise() - p_mean;

  std::vector<double> vertexErrors(N, 0.0);
  double totalMaxError = 0.0;

  // 2. Analyze motion across all frames
  for (const auto& pose : allPoses)
  {
    // Build Target Pose matrix for this frame
    Eigen::Matrix3Xd Q(3, N);
    for (size_t i = 0; i < N; ++i)
    {
      Q.col(i) = pose.col(indices[i]);
    }

    const Eigen::Vector3d q_mean = Q.rowwise().mean();
    const Eigen::Matrix3Xd Q_centered = Q.colwise() - q_mean;

    // Solve Rotation R using SVD
    Eigen::Matrix3d H = Q_centered * P_centered.transpose();
    Eigen::JacobiSVD<Eigen::Matrix3d> svd(H, Eigen::ComputeFullU | Eigen::ComputeFullV);

    Eigen::Matrix3d U = svd.matrixU();
    Eigen::Matrix3d V = svd.matrixV();
    Eigen::Matrix3d R = U * V.transpose();

    if (R.determinant() < 0)
    {
      V.col(2) *= -1.0;
      R = U * V.transpose();
    }
    const Eigen::Vector3d T = q_mean - R * p_mean;

    // Accumulate Error: How much does each vertex "fight" the rigid motion?
    for (size_t i = 0; i < N; ++i)
    {
      Eigen::Vector3d expected = R * P.col(i) + T;
      double err = (expected - Q.col(i)).squaredNorm();
      vertexErrors[i] += err;
      totalMaxError = std::max(totalMaxError, vertexErrors[i]);
    }
  }

  // 3. Final Weighted Centroid
  Eigen::Vector3d weightedCentroid = Eigen::Vector3d::Zero();
  double totalWeight = 0.0;

  for (size_t i = 0; i < N; ++i)
  {
    // Rigid vertices (low error) get weight 1.0.
    // Stretchy vertices (high error) get weight near 0.0.
    // sigma prevents division by zero and controls the "falloff"
    double sigma = (totalMaxError / 10.0) + 1e-7;
    double w = 1.0 / (1.0 + (vertexErrors[i] / sigma));

    weightedCentroid += P.col(i) * w;
    totalWeight += w;
  }

  return weightedCentroid / totalWeight;
}

/**
 * Calculates the 3D 'Pivot Point' for every discovered bone.
 *
 * For each bone group, it finds the geometric center (centroid) of its
 * assigned vertices in the rest pose. These points act as the 'Joints'
 * for the glTF skeleton and the origin for all rotations.
 */
std::vector<Eigen::Vector3d> ComputeBoneRestPositions(const std::vector<std::vector<int>>& groups,
                                                      const std::vector<Eigen::Matrix3Xd>& allPoses,
                                                      const Eigen::Matrix3Xd& restPose)
{
  std::vector<Eigen::Vector3d> bonePositions;
  for (const auto& groupIndices : groups)
  {
    // Use the new solver here
    bonePositions.push_back(ComputeRobustCentroid(groupIndices, allPoses, restPose));
  }
  return bonePositions;
}

/**
 * Removes 'parasitic' weights by enforcing physical connectivity.
 *
 * Sometimes two distant parts (like a hand and a foot) move in sync, causing
 * the math to group them together. This function finds 'islands' of vertices
 * that aren't physically touching the main body of a bone and 'snaps' them
 * back to their actual physical neighbors.
 */
void CleanBoneIslands(std::vector<int>& assignments, const Eigen::MatrixXi& F, int numVertices,
                      int numBones)
{
  // 1. Build Adjacency
  std::vector<std::vector<int>> adj(numVertices);
  for (int i = 0; i < F.rows(); ++i)
  {
    for (int j = 0; j < 3; ++j)
    {
      int u = F(i, j);
      int v = F(i, (j + 1) % 3);
      adj[u].push_back(v);
      adj[v].push_back(u);
    }
  }

  // 2. Group into islands
  std::vector<int> islandID(numVertices, -1);
  std::vector<std::vector<int>> islands;
  std::vector<int> islandBoneType;

  std::vector<bool> visited(numVertices, false);
  for (int i = 0; i < numVertices; ++i)
  {
    if (visited[i])
      continue;

    int currentBone = assignments[i];
    std::vector<int> component;
    std::queue<int> q;
    q.push(i);
    visited[i] = true;

    while (!q.empty())
    {
      int u = q.front();
      q.pop();
      component.push_back(u);
      islandID[u] = (int)islands.size();
      for (int v : adj[u])
      {
        if (!visited[v] && assignments[v] == currentBone)
        {
          visited[v] = true;
          q.push(v);
        }
      }
    }
    islandBoneType.push_back(currentBone);
    islands.push_back(component);
  }

  // 3. Find the "Alpha" island for each bone (the largest one)
  std::vector<int> alphaIslandForBone(numBones, -1);
  std::vector<size_t> maxSizes(numBones, 0);
  for (std::size_t i = 0; i < islands.size(); i++)
  {
    int b = islandBoneType[i];
    if (islands[i].size() > maxSizes[b])
    {
      maxSizes[b] = islands[i].size();
      alphaIslandForBone[b] = i;
    }
  }

  // 4. Snap Orphans: If you're not in your bone's alpha island,
  // take the bone ID of a neighboring vertex that IS in an alpha island.
  for (int i = 0; i < numVertices; i++)
  {
    int myIsland = islandID[i];
    int myBone = assignments[i];

    if (myIsland != alphaIslandForBone[myBone])
    {
      // I'm an orphan! Find a neighbor to adopt me.
      for (int neighbor : adj[i])
      {
        int nIsland = islandID[neighbor];
        int nBone = assignments[neighbor];
        // Only adopt if the neighbor is part of a "legit" main bone body
        if (nIsland == alphaIslandForBone[nBone])
        {
          assignments[i] = nBone;
          break;
        }
      }
    }
  }
}

// Represents a frame's motion profile
struct FrameSignature
{
  int frame_id;
  Eigen::VectorXd signature;  // Flattened normalized displacements
};

/**
 * Creates a normalized motion signature for every frame.
 * We subtract the rest pose to isolate movement.
 */
std::vector<FrameSignature> GenerateMotionSignatures(const std::vector<Eigen::Matrix3Xd>& allPoses,
                                                     const Eigen::Matrix3Xd& restPose)
{
  const int numFrames = allPoses.size();
  const int numVerts = restPose.cols();
  std::vector<FrameSignature> signatures(numFrames);

  for (int f = 0; f < numFrames; ++f)
  {
    signatures[f].frame_id = f;

    // 1. Calculate displacement from rest pose
    Eigen::Matrix3Xd displacement = allPoses[f] - restPose;

    // 2. Flatten to a 1D vector (3 * N)
    Eigen::VectorXd flat = Eigen::Map<const Eigen::VectorXd>(displacement.data(), numVerts * 3);

    // 3. Normalize: This ensures "subtle neck tilt" and "big run"
    // are compared by WHERE the motion happens, not just how big it is.
    double norm = flat.norm();
    if (norm > 1e-8)
    {
      signatures[f].signature = flat / norm;
    }
    else
    {
      signatures[f].signature = Eigen::VectorXd::Zero(numVerts * 3);
    }
  }
  return signatures;
}

/**
 * Clusters frames based on a similarity threshold (0.0 to 1.0).
 *
 * @return A map where:
 *   - Key (int): The Unique Bucket ID.
 *   - Value (std::vector<int>): A list of Frame IDs that belong to this bucket.
 *
 * Example:
 *   Bucket 0 might contain [0, 1, 2, 50, 51] (The "Thinking" idle)
 *   Bucket 1 might contain [10, 11, 12, 13] (The "Run" cycle)
 */
std::map<int, std::vector<int>> AutoBucketizeFrames(const std::vector<FrameSignature>& signatures,
                                                    double similarity_threshold = 0.9)
{
  // Key: Bucket Index, Value: List of Frame IDs
  std::map<int, std::vector<int>> buckets;
  if (signatures.empty())
    return buckets;

  // "Leaders" are the reference signatures that define what a bucket "looks" like.
  std::vector<Eigen::VectorXd> leader_signatures;

  for (const auto& sig : signatures)
  {
    bool assigned = false;

    for (size_t l = 0; l < leader_signatures.size(); ++l)
    {
      // Since signatures are normalized, the Dot Product is the Cosine Similarity.
      // 1.0 = Perfect match, 0.0 = Completely different motion.
      double similarity = sig.signature.dot(leader_signatures[l]);

      if (similarity > similarity_threshold)
      {
        buckets[static_cast<int>(l)].push_back(sig.frame_id);
        assigned = true;
        break;
      }
    }

    if (!assigned)
    {
      // No existing bucket matches this motion. Start a new one!
      int new_id = static_cast<int>(leader_signatures.size());
      leader_signatures.push_back(sig.signature);
      buckets[new_id].push_back(sig.frame_id);
    }
  }
  return buckets;
}

/**
 * Executes the splitting logic independently for each motion bucket.
 * This isolates subtle movements (like head tilts) from loud movements (like running).
 */
std::map<int, std::vector<int>>
GenerateBucketProposals(const std::map<int, std::vector<int>>& frame_buckets,
                        const std::vector<Eigen::Matrix3Xd>& allPoses,
                        const Eigen::Matrix3Xd& restPose, const SimpleGeodesic::HeatGeodesicsData&)
{
  std::map<int, std::vector<int>> proposals;

  for (auto const& [bucket_id, frame_ids] : frame_buckets)
  {
    // 1. Extract only the frames belonging to this specific motion type
    std::vector<Eigen::Matrix3Xd> bucket_poses;
    for (int f_id : frame_ids)
    {
      bucket_poses.push_back(allPoses[f_id]);
    }

    // 2. Run the Adaptive Splitter on this subset.
    // We use a tighter tolerance because the "noise" of other animations is gone.
    const double bucket_tolerance = 0.005;
    proposals[bucket_id] = CalculateVertexGroupsAdaptive(bucket_poses, restPose, bucket_tolerance);

    INFO_LOG_FMT(VIDEO, "Bucket {}: Produced {} unique motion clusters for {} frames.", bucket_id,
                 bucket_poses.size(), frame_ids.size());
  }
  return proposals;
}

/**
 * Merges multiple animation-specific bone proposals into a single global bone map.
 *
 * Logic: Every vertex gets a "Signature" based on its bone ID in every bucket.
 * If two vertices have the exact same signature, they are the same bone.
 * If they differ in even ONE bucket, they are split into separate bones.
 *
 * @return A vector of final bone assignments (one per vertex).
 */
std::vector<int> FinalMergeProposals(int numVertices,
                                     const std::map<int, std::vector<int>>& bucket_proposals)
{
  // Map of "Motion Signature" -> "New Unique Bone ID"
  std::map<std::string, int> signature_to_final_id;
  std::vector<int> final_assignments(numVertices);
  int next_bone_id = 0;

  for (int i = 0; i < numVertices; ++i)
  {
    // Build the signature. The map iteration order (bucket_id 0, 1, 2...)
    // ensures consistency across all vertices.
    std::string signature = "";
    for (auto const& [bucket_id, assignments] : bucket_proposals)
    {
      signature += std::to_string(assignments[i]) + "|";
    }

    // If this specific combination of motions hasn't been seen, register it.
    if (signature_to_final_id.find(signature) == signature_to_final_id.end())
    {
      signature_to_final_id[signature] = next_bone_id++;
    }

    final_assignments[i] = signature_to_final_id[signature];
  }

  INFO_LOG_FMT(VIDEO, "Merging complete. Discovered {} total bones across all animation buckets.",
               next_bone_id);
  return final_assignments;
}

/**
 * Reduces over-segmented bone groups into a stable, functional rig.
 *
 * Strategy:
 * 1. Topological Adjacency: Only attempts to merge bones that share a mesh edge.
 * 2. Motion Fingerprinting: Skips expensive SSE math if bone trajectories differ greatly.
 * 3. Priority Noise Reduction: Automatically absorbs "micro-bones" into neighbors.
 * 4. Union-Find Safety: Uses path compression (find_root) to prevent infinite remap loops.
 */
std::vector<int>
PruneAndMergeBones(const std::vector<int>& initial_assignments,
                   const std::vector<Eigen::Matrix3Xd>& allPoses, const Eigen::Matrix3Xd& restPose,
                   const Eigen::MatrixXi& F,        // Pass the faces for adjacency!
                   double merge_tolerance = 0.005)  // Loosened from 0.0005 to catch more noise
{
  const int numVertices = static_cast<int>(initial_assignments.size());
  int numBones = 0;
  for (int b : initial_assignments)
    numBones = std::max(numBones, b + 1);

  // 1. Group indices by bone
  std::vector<std::vector<int>> bone_to_indices(numBones);
  for (int i = 0; i < numVertices; ++i)
  {
    bone_to_indices[initial_assignments[i]].push_back(i);
  }

  // 2. SPEED HACK: Build Adjacency Map
  // Instead of comparing all 216 * 216 pairs, we only compare neighbors.
  std::set<std::pair<int, int>> bone_neighbors;
  for (int i = 0; i < F.rows(); ++i)
  {
    int b[3] = {initial_assignments[F(i, 0)], initial_assignments[F(i, 1)],
                initial_assignments[F(i, 2)]};
    for (int j = 0; j < 3; ++j)
    {
      int u = b[j], v = b[(j + 1) % 3];
      if (u != v)
        bone_neighbors.insert({std::min(u, v), std::max(u, v)});
    }
  }

  // 3. Union-Find Helper
  std::vector<int> bone_remap(numBones);
  std::iota(bone_remap.begin(), bone_remap.end(), 0);
  auto find_root = [&](int b) {
    int root = b;
    while (bone_remap[root] != root)
      root = bone_remap[root];
    return root;
  };

  // 4. Compare only NEIGHBORING pairs
  for (auto const& pair : bone_neighbors)
  {
    int a = find_root(pair.first);
    int b = find_root(pair.second);
    if (a == b)
      continue;

    // YOUR ORIGINAL SSE LOGIC:
    std::vector<int> combined = bone_to_indices[a];
    combined.insert(combined.end(), bone_to_indices[b].begin(), bone_to_indices[b].end());

    double error_a = CalculateRigidSSE_WorldSpace(bone_to_indices[a], allPoses, restPose);
    double error_b = CalculateRigidSSE_WorldSpace(bone_to_indices[b], allPoses, restPose);
    double error_combined = CalculateRigidSSE_WorldSpace(combined, allPoses, restPose);

    double merge_cost = error_combined - (error_a + error_b);

    if (merge_cost < merge_tolerance)
    {
      INFO_LOG_FMT(VIDEO, "Merging bone {} into {}. Cost: {}", b, a, merge_cost);
      bone_remap[b] = a;
      bone_to_indices[a] = combined;  // Update 'a' so subsequent checks are accurate
      bone_to_indices[b].clear();
    }
  }

  // 5. Final Pass: Re-index
  std::vector<int> final_assignments(numVertices);
  std::map<int, int> compact_map;
  int next_id = 0;
  for (int i = 0; i < numVertices; ++i)
  {
    int root = find_root(initial_assignments[i]);
    if (compact_map.find(root) == compact_map.end())
      compact_map[root] = next_id++;
    final_assignments[i] = compact_map[root];
  }

  INFO_LOG_FMT(VIDEO, "Reduced {} bones to {} bones.", numBones, next_id);
  return final_assignments;
}

/**
 * The high-level orchestrator for the new bucket-driven rigging system.
 */
std::vector<int> GenerateRigFromMotion(const std::vector<Eigen::Matrix3Xd>& allPoses,
                                       const Eigen::Matrix3Xd& restPose,
                                       const SimpleGeodesic::HeatGeodesicsData& heat_data)
{
  // 1. PHASE ONE: Signature Generation
  // Normalizes all frames so subtle motions (breathing) match the intensity of loud ones
  // (punching).
  auto signatures = GenerateMotionSignatures(allPoses, restPose);

  // 2. PHASE TWO: Auto-Bucketization
  // Discovers how many "distinct" animations are in the capture (Ninja flips, thinking, etc).
  // Similarity of 0.9 is a good "high sensitivity" start.
  auto frame_buckets = AutoBucketizeFrames(signatures, 0.9);
  INFO_LOG_FMT(VIDEO, "Detected {} distinct motion buckets.", frame_buckets.size());

  // 3. PHASE THREE: Independent Proposals
  // Each bucket proposes its own internal bones based ONLY on its local motion.
  // This is where the Neck finally gets its own bone because the "Idle" bucket sees it.
  auto proposals = GenerateBucketProposals(frame_buckets, allPoses, restPose, heat_data);

  // 4. PHASE FOUR: The "Union" Merge
  // Combines all bucket proposals into a high-resolution "Signature Map".
  // This will likely over-segment (e.g., 80 bones), but it won't MISS any joints.
  auto raw_assignments = FinalMergeProposals(restPose.cols(), proposals);

  // 5. PHASE FIVE: SSE Pruning (The Skeptic)
  // Collapses "mathematical noise" bones back together if they move identically across ALL
  // frames. This is your safety net against "patchy" micro-bones.
  double pruning_tolerance = 0.0005;  // Tight tolerance to keep the neck!
  auto pruned_assignments =
      PruneAndMergeBones(raw_assignments, allPoses, restPose, heat_data.F, pruning_tolerance);

  // 6. PHASE SIX: Final Sanitization
  // Use your existing BFS "Island Cleaner" to ensure bones are contiguous.
  int final_bone_count = 0;
  for (int b : pruned_assignments)
    final_bone_count = std::max(final_bone_count, b + 1);

  CleanBoneIslands(pruned_assignments, heat_data.F, restPose.cols(), final_bone_count);

  return pruned_assignments;
}

/**
 * Validates the skeleton by rebuilding the animation using only bones and weights.
 *
 * This is the 'Proof of Work'. It applies the calculated Rigid Transforms (R, T)
 * to the Rest Pose vertices using the smooth Weights (W). If the result matches
 * the original animation, the bone discovery and skinning are successful.
 */
std::vector<std::vector<int>> ReconstructGroups(const std::vector<int>& cleanAssignments,
                                                int numBones)
{
  std::vector<std::vector<int>> groups(numBones);
  for (std::size_t vIdx = 0; vIdx < cleanAssignments.size(); ++vIdx)
  {
    int boneID = cleanAssignments[vIdx];
    if (boneID >= 0 && boneID < numBones)
    {
      groups[boneID].push_back(vIdx);
    }
  }
  return groups;
}

std::vector<GraphicsModEditor::VertexInfluence> FindTopInfluences(const Eigen::MatrixXd& W)
{
  const int N = static_cast<int>(W.rows());
  const int B = static_cast<int>(W.cols());
  std::vector<GraphicsModEditor::VertexInfluence> influences(N);

  // Noise Filter Threshold
  // Any bone with less than this influence is likely "mathematical noise"
  const double noise_floor = 0.02;

  for (int i = 0; i < N; ++i)
  {
    // 1. Collect all candidates and apply Noise Filter
    std::vector<std::pair<int, double>> pairs;
    for (int j = 0; j < B; ++j)
    {
      double weight = W(i, j);

      // If the influence is too weak, drop it entirely to stabilize the mesh
      if (weight < noise_floor)
      {
        weight = 0.0;
      }

      pairs.push_back({j, weight});
    }

    // 2. Sort descending
    std::sort(pairs.begin(), pairs.end(),
              [](const auto& a, const auto& b) { return a.second > b.second; });

    // 3. Extract Top 4 and sum them for re-normalization
    double sum = 0.0;
    for (int j = 0; j < 4; ++j)
    {
      if (j < (int)pairs.size() && pairs[j].second > 0.0)
      {
        influences[i].bone_ids[j] = pairs[j].first;
        influences[i].weights[j] = static_cast<float>(pairs[j].second);
        sum += pairs[j].second;
      }
      else
      {
        // Default to bone 0, weight 0.0 for empty slots
        influences[i].bone_ids[j] = 0;
        influences[i].weights[j] = 0.0f;
      }
    }

    // 4. Final Truncation Normalization
    // This is CRITICAL: After killing the noise, we must re-normalize
    // so the remaining "Strong" bones sum to 1.0.
    if (sum > 1e-6)
    {
      for (int j = 0; j < 4; ++j)
      {
        influences[i].weights[j] /= static_cast<float>(sum);
      }
    }
    else
    {
      // Fallback: If ALL weights were noise, snap to the strongest original bone
      // This prevents "invisible" vertices.
      int strongest_bone = pairs[0].first;
      influences[i].bone_ids[0] = strongest_bone;
      influences[i].weights[0] = 1.0f;
      for (int j = 1; j < 4; ++j)
      {
        influences[i].bone_ids[j] = 0;
        influences[i].weights[j] = 0.0f;
      }
    }
  }
  return influences;
}

/**
 * Connects bones into a skeletal tree.
 * 1. Finds the "most significant" bone (Torso) to act as Root.
 * 2. Uses Mesh Topology (Shared Edges) to build the primary tree.
 * 3. Uses Physical Distance as a fallback for floating/disconnected parts.
 */
std::vector<int> BuildBoneHierarchyForGLTF(const std::vector<int>& assignments,
                                           const Eigen::MatrixXi& F,
                                           const std::vector<Eigen::Vector3d>& boneCenters,
                                           int numBones)
{
  // --- 1. Count shared edges between bones ---
  Eigen::MatrixXd adj = Eigen::MatrixXd::Zero(numBones, numBones);
  for (int i = 0; i < F.rows(); ++i)
  {
    for (int j = 0; j < 3; ++j)
    {
      int u = assignments[F(i, j)];
      int v = assignments[F(i, (j + 1) % 3)];
      if (u >= 0 && v >= 0 && u != v)
      {
        adj(u, v)++;
        adj(v, u)++;
      }
    }
  }

  // --- 2. Find the Root Bone (The one with the most vertices) ---
  std::vector<int> boneSizes(numBones, 0);
  for (int b : assignments)
    if (b >= 0)
      boneSizes[b]++;

  int rootBone = 0;
  int maxVerts = -1;
  for (int i = 0; i < numBones; ++i)
  {
    if (boneSizes[i] > maxVerts)
    {
      maxVerts = boneSizes[i];
      rootBone = i;
    }
  }

  // --- 3. Build the Tree (Prim's Algorithm / MST) ---
  std::vector<int> parents(numBones, -1);
  std::vector<bool> visited(numBones, false);
  visited[rootBone] = true;

  for (int iteration = 0; iteration < numBones - 1; ++iteration)
  {
    int bestU = -1;  // Existing parent
    int bestV = -1;  // New child
    double maxShared = -1.0;

    // First pass: Try to connect via shared mesh edges (Topological)
    for (int u = 0; u < numBones; ++u)
    {
      if (!visited[u])
        continue;
      for (int v = 0; v < numBones; ++v)
      {
        if (visited[v])
          continue;
        if (adj(u, v) > maxShared)
        {
          maxShared = adj(u, v);
          bestU = u;
          bestV = v;
        }
      }
    }

    // Second pass: If no shared edges found, connect by distance (Geometric Fallback)
    // This handles floating eyes, belts, or disconnected props.
    if (bestV == -1 || maxShared <= 0)
    {
      double minDistance = std::numeric_limits<double>::max();
      for (int u = 0; u < numBones; ++u)
      {
        if (!visited[u])
          continue;
        for (int v = 0; v < numBones; ++v)
        {
          if (visited[v])
            continue;
          double dist = (boneCenters[u] - boneCenters[v]).norm();
          if (dist < minDistance)
          {
            minDistance = dist;
            bestU = u;
            bestV = v;
          }
        }
      }
    }

    if (bestV != -1)
    {
      parents[bestV] = bestU;
      visited[bestV] = true;
    }
  }

  return parents;
}

template <typename T>
int AddBufferToModel(tinygltf::Model& model, const std::vector<T>& data, int target, int type,
                     int componentType)
{
  size_t byteLength = data.size() * sizeof(T);
  size_t bufferOffset = model.buffers.empty() ? 0 : model.buffers[0].data.size();

  if (model.buffers.empty())
    model.buffers.emplace_back();
  model.buffers[0].data.resize(bufferOffset + byteLength);
  std::memcpy(model.buffers[0].data.data() + bufferOffset, data.data(), byteLength);

  tinygltf::BufferView view;
  view.buffer = 0;
  view.byteOffset = bufferOffset;
  view.byteLength = byteLength;
  view.target = target;
  model.bufferViews.push_back(view);

  tinygltf::Accessor accessor;
  accessor.bufferView = static_cast<int>(model.bufferViews.size() - 1);
  accessor.byteOffset = 0;
  accessor.componentType = componentType;

  if (type == TINYGLTF_TYPE_SCALAR)
  {
    accessor.count = data.size();
  }
  else if (type == TINYGLTF_TYPE_VEC3)
  {
    accessor.count = data.size() / 3;
  }
  else if (type == TINYGLTF_TYPE_VEC4)
  {
    accessor.count = data.size() / 4;
  }
  else if (type == TINYGLTF_TYPE_MAT4)
  {
    accessor.count = data.size() / 16;
  }

  accessor.type = type;
  model.accessors.push_back(accessor);

  return static_cast<int>(model.accessors.size() - 1);
}

void ExportToGLTF(const std::string& path, const Eigen::Matrix3Xd& V, const Eigen::MatrixXi& F,
                  const std::vector<Eigen::Vector3d>& boneCenters,
                  const std::vector<GraphicsModEditor::VertexInfluence>& influences)
{
  tinygltf::Model model;
  tinygltf::Asset asset;
  asset.version = "2.0";
  model.asset = asset;

  // 1. Prepare Data for Packing
  std::vector<float> posData;
  for (int i = 0; i < V.cols(); ++i)
  {
    posData.push_back((float)V(0, i));
    posData.push_back((float)V(1, i));
    posData.push_back((float)V(2, i));
  }

  std::vector<uint32_t> indexData;
  for (int i = 0; i < F.rows(); ++i)
  {
    indexData.push_back((uint32_t)F(i, 0));
    indexData.push_back((uint32_t)F(i, 1));
    indexData.push_back((uint32_t)F(i, 2));
  }

  std::vector<u16> bone_data;
  std::vector<float> weight_data;
  for (const auto& inf : influences)
  {
    for (int j = 0; j < 4; ++j)
    {
      bone_data.push_back(static_cast<u16>(inf.bone_ids[j]));
      weight_data.push_back(inf.weights[j]);
    }
  }

  // 2. Pack Buffers
  const int posAcc =
      AddBufferToModel(model, posData, 34962, TINYGLTF_TYPE_VEC3, TINYGLTF_COMPONENT_TYPE_FLOAT);
  const int idxAcc = AddBufferToModel(model, indexData, 34963, TINYGLTF_TYPE_SCALAR,
                                      TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT);
  const int jntAcc = AddBufferToModel(model, bone_data, 34962, TINYGLTF_TYPE_VEC4,
                                      TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT);
  const int wgtAcc = AddBufferToModel(model, weight_data, 34962, TINYGLTF_TYPE_VEC4,
                                      TINYGLTF_COMPONENT_TYPE_FLOAT);

  std::vector<float> ibmData;
  for (const auto& center : boneCenters)
  {
    // A matrix that translates by -center
    Eigen::Matrix4d invBind = Eigen::Matrix4d::Identity();
    invBind(0, 3) = -center.x();
    invBind(1, 3) = -center.y();
    invBind(2, 3) = -center.z();

    // Pack 16 floats (column-major)
    for (int col = 0; col < 4; ++col)
    {
      for (int row = 0; row < 4; ++row)
      {
        ibmData.push_back((float)invBind(row, col));
      }
    }
  }

  // 3. Build Mesh
  tinygltf::Mesh mesh;
  tinygltf::Primitive prim;
  prim.attributes["POSITION"] = posAcc;
  prim.attributes["JOINTS_0"] = jntAcc;
  prim.attributes["WEIGHTS_0"] = wgtAcc;
  prim.indices = idxAcc;
  prim.mode = 4;  // Triangles
  mesh.primitives.push_back(prim);
  model.meshes.push_back(mesh);

  // 4. Create Bone Nodes and Skin
  tinygltf::Skin skin;

  const int ibmAcc =
      AddBufferToModel(model, ibmData, 0, TINYGLTF_TYPE_MAT4, TINYGLTF_COMPONENT_TYPE_FLOAT);
  skin.inverseBindMatrices = ibmAcc;

  tinygltf::Scene scene;
  tinygltf::Node meshNode;
  meshNode.mesh = 0;
  meshNode.skin = 0;
  model.nodes.push_back(meshNode);
  scene.nodes.push_back(0);

  for (std::size_t i = 0; i < boneCenters.size(); ++i)
  {
    tinygltf::Node bone;
    bone.name = "Bone_" + std::to_string(i);
    bone.translation = {boneCenters[i].x(), boneCenters[i].y(), boneCenters[i].z()};
    model.nodes.push_back(bone);

    int boneNodeIdx = static_cast<int>(model.nodes.size());
    model.nodes.push_back(bone);
    skin.joints.push_back(boneNodeIdx);
    scene.nodes.push_back(boneNodeIdx);
  }
  model.skins.push_back(skin);

  model.scenes.push_back(scene);
  model.defaultScene = 0;

  // 5. Save
  tinygltf::TinyGLTF writer;
  writer.WriteGltfSceneToFile(&model, path, true, true, true, false);
}

void ExportRigBinary(
    File::IOFile* file_data,
    const Eigen::MatrixXd& V_welded,    // Unique Positions [3 x N]
    const Eigen::VectorXi& global_SVJ,  // Map: UnweldedIdx -> WeldedIdx
    const std::vector<GraphicsModEditor::VertexInfluence>& vertex_influences,
    const std::vector<Eigen::Vector3d>& bone_centers,
    const std::map<GraphicsModSystem::DrawCallID, GraphicsModEditor::VertexGroupDumper::FinalData>&
        draw_call_to_data,
    const Eigen::Vector3d& mesh_centroid, double mesh_scale)
{
  GraphicsModEditor::ExporterSkinningRig export_rig;

  // 1. Convert Global Welded Cage
  // If V_welded is [3 x N], .col(i) is correct.
  export_rig.runtime_rig.welded_positions.reserve(V_welded.cols());
  for (int i = 0; i < V_welded.cols(); ++i)
  {
    export_rig.runtime_rig.welded_positions.push_back(V_welded.col(i).cast<float>());
  }

  // 2. Bone Centers & Weights
  export_rig.global_weights = vertex_influences;
  for (const auto& bc_double : bone_centers)
  {
    export_rig.runtime_rig.bone_rest_centers.push_back(bc_double.cast<float>());
  }

  // 3. Chunk-Specific Slicing (SVJ + Bone Groups)
  std::size_t global_v_offset = 0;
  for (auto const& [draw_call, data] : draw_call_to_data)
  {
    const u32 v_count = static_cast<u32>(data.original_vertex_count);

    // --- PART A: Local SVJ (Native -> Welded Map) ---
    auto& local_svj = export_rig.draw_call_to_local_svj[draw_call];
    local_svj.reserve(v_count);
    for (u32 i = 0; i < v_count; ++i)
    {
      // global_SVJ is a VectorXi, so we just use () or []
      local_svj.push_back(global_SVJ(global_v_offset + i));
    }

    // --- PART B: SVD Tracking Groups (ChunkRigData) ---
    // This tells the SVD solver: "In this chunk, which vertices belong to which bone?"
    VideoCommon::ChunkRigData& chunk_rig = export_rig.runtime_rig.draw_call_rig_details[draw_call];
    chunk_rig.draw_call_id = draw_call;

    for (u32 local_i = 0; local_i < v_count; ++local_i)
    {
      int welded_idx = local_svj[local_i];
      const GraphicsModEditor::VertexInfluence& inf = vertex_influences[welded_idx];

      // Check all 4 baked influences for this vertex
      for (int k = 0; k < 4; ++k)
      {
        float w = inf.weights[k];
        // Only track vertices with significant weight to keep SVD stable
        if (w > 0.1f)
        {
          int b_id = inf.bone_ids[k];
          auto& group = chunk_rig.bone_groups[b_id];
          group.bone_id = b_id;

          // PAIR CORRESPONDENCE:
          // We need the moving vertex (local_i) and the rest vertex (welded_idx)
          group.original_indices.push_back(static_cast<int>(local_i));
          group.welded_indices.push_back(welded_idx);
          group.weights.push_back(w);  // Confidence score for SVD
        }
      }
    }

    global_v_offset += v_count;
  }

  // The scale/centroid
  export_rig.runtime_rig.welded_rig_scale = static_cast<float>(mesh_scale);
  export_rig.runtime_rig.welded_rig_centroid = mesh_centroid.cast<float>();

  GraphicsModEditor::ExporterSkinningRig::ToBinary(file_data, export_rig);
}

}  // namespace

namespace GraphicsModEditor
{
bool VertexGroupDumper::IsDrawCallInRecording(GraphicsModSystem::DrawCallID draw_call_id) const
{
  if (!m_recording_request.has_value())
    return false;

  return m_recording_request->m_draw_call_ids.contains(draw_call_id);
}

void VertexGroupDumper::AddDataToRecording(GraphicsModSystem::DrawCallID draw_call_id,
                                           const GraphicsModSystem::DrawDataView& draw_data)
{
  auto current_vertex_positions = ReadMeshPositions(draw_data);

  Eigen::Matrix3Xd matrix_positions = ConvertMeshPositions(current_vertex_positions, draw_data);
  auto& data = m_draw_call_to_data[draw_call_id];

  if (IsDuplicateMesh(matrix_positions, data.mesh_poses))
    return;

  if (data.original_vertex_count == 0)
  {
    data.original_vertex_count = current_vertex_positions.size();

    if (data.original_vertex_count == 0)
      return;

    if (draw_data.uid->rasterization_state.primitive == PrimitiveType::TriangleStrip)
    {
      std::vector<int> indices;
      u32 strip_count = 0;
      for (std::size_t i = 0; i < draw_data.index_data.size(); i++)
      {
        // Primitive restart
        if (draw_data.index_data[i] == UINT16_MAX)
        {
          strip_count = 0;  // Reset strip state
          continue;
        }

        // A triangle is at least 3 verts
        strip_count++;
        if (strip_count < 3)
          continue;

        u16 i0, i1, i2;

        // Triangle index 'n' in the current strip is at (i-2, i-1, i)
        // Its local index is (triangleCountInStrip - 3)
        if ((strip_count - 3) % 2 == 0)
        {
          i0 = draw_data.index_data[i - 2];
          i1 = draw_data.index_data[i - 1];
          i2 = draw_data.index_data[i];
        }
        else
        {
          // Swap for odd-numbered triangles to maintain consistent CCW winding
          i0 = draw_data.index_data[i - 2];
          i1 = draw_data.index_data[i];
          i2 = draw_data.index_data[i - 1];
        }

        if (i0 == i1 || i1 == i2 || i0 == i2)
        {
          continue;
        }

        indices.push_back(i0);
        indices.push_back(i1);
        indices.push_back(i2);
      }

      const std::size_t num_triangles = indices.size() / 3;
      Eigen::MatrixXi F(num_triangles, 3);

      for (int i = 0; i < num_triangles; ++i)
      {
        F(i, 0) = indices[i * 3 + 0];
        F(i, 1) = indices[i * 3 + 1];
        F(i, 2) = indices[i * 3 + 2];
      }
      data.rest_pose_face_indexes = std::move(F);
    }
    else
    {
      const std::size_t num_triangles = draw_data.index_data.size() / 3;
      Eigen::MatrixXi F(num_triangles, 3);

      for (int i = 0; i < num_triangles; ++i)
      {
        F(i, 0) = draw_data.index_data[i * 3 + 0];
        F(i, 1) = draw_data.index_data[i * 3 + 1];
        F(i, 2) = draw_data.index_data[i * 3 + 2];
      }

      data.rest_pose_face_indexes = std::move(F);
    }
  }

  m_current_xfb_data.emplace_back(draw_call_id, std::move(matrix_positions));
}

void VertexGroupDumper::OnXFBCreated(const std::string& hash)
{
  if (!m_recording_request.has_value())
    return;

  m_xfb_to_draw_data[hash] = std::move(m_current_xfb_data);
}

void VertexGroupDumper::OnFramePresented(std::span<std::string> xfbs_presented)
{
  if (m_xfb_to_draw_data.empty())
    return;

  for (const auto& xfb : xfbs_presented)
  {
    if (const auto iter = m_xfb_to_draw_data.find(xfb); iter != m_xfb_to_draw_data.end())
    {
      for (auto& [draw_call, data] : iter->second)
      {
        auto& final_data = m_draw_call_to_data[draw_call];
        final_data.mesh_poses.push_back(std::move(data));
        final_data.frame_id_to_mesh_index[m_frame_id] = final_data.mesh_poses.size() - 1;
      }
      m_xfb_to_draw_data.erase(iter);
    }
  }

  // If our recording is done and we have no data left to capture
  if (m_xfb_to_draw_data.empty() && !m_recording_request.has_value())
  {
    Save();
    m_draw_call_to_data.clear();
    m_frame_id = 0;
    return;
  }

  m_frame_id++;
}

void VertexGroupDumper::Record(const VertexGroupRecordingRequest& request)
{
  m_recording_request = request;
}

void VertexGroupDumper::StopRecord()
{
  m_recording_request.reset();
}

bool VertexGroupDumper::IsRecording() const
{
  return m_recording_request.has_value();
}

void VertexGroupDumper::Save()
{
  if (m_draw_call_to_data.empty()) [[unlikely]]
    return;

  Eigen::MatrixXi F_global;
  std::size_t v_offset = 0;
  for (const auto& [draw_call, data] : m_draw_call_to_data)
  {
    F_global.conservativeResize(F_global.rows() + data.rest_pose_face_indexes.rows(), 3);
    F_global.bottomRows(data.rest_pose_face_indexes.rows()) =
        data.rest_pose_face_indexes.array() + v_offset;
    v_offset += data.original_vertex_count;  // Number of vertices in THIS chunk
  }

  auto reassembled_data = ReassembleSignificantFrames(m_draw_call_to_data);

  std::vector<GraphicsModSystem::DrawCallID> vertex_to_draw_call;
  for (const auto& [draw_call, data] : m_draw_call_to_data)
  {
    const int num_verts = static_cast<int>(data.mesh_poses[0].cols());

    for (int j = 0; j < num_verts; ++j)
    {
      vertex_to_draw_call.push_back(draw_call);
    }
  }

  double mesh_scale;
  Eigen::Vector3d mesh_centroid;
  NormalizeWeldedAnimation(reassembled_data, mesh_scale, mesh_centroid);

  Eigen::MatrixXd V_raw = reassembled_data[0].transpose();  // Nx3

  // 2. Find the Gap (make sure this is correct)
  const double weld_distance = FindMinimumChunkGap(reassembled_data[0], vertex_to_draw_call);

  ERROR_LOG_FMT(VIDEO, "Weld distance: {}", weld_distance);

  // 3. WELD EVERYTHING
  Eigen::MatrixXd SV;  // Unique Vertices
  Eigen::VectorXi SVI, SVJ;
  Eigen::MatrixXi SF;
  double final_epsilon = std::max(0.001, weld_distance);
  weld_mesh_euclidean(reassembled_data[0].transpose(), F_global, final_epsilon, SV, SVI, SVJ, SF);

  WARN_LOG_FMT(VIDEO, "[MeshDebug] Welded vertex count: {} | Welded face count: {}", SV.rows(),
               SF.rows());

  for (auto& pose : reassembled_data)
  {
    Eigen::Matrix3Xd p = Eigen::Matrix3Xd::Zero(3, SV.rows());
    Eigen::VectorXd counts = Eigen::VectorXd::Zero(SV.rows());
    for (int i = 0; i < pose.cols(); ++i)
    {
      int uIdx = SVJ(i);
      p.col(uIdx) += pose.col(i);
      counts(uIdx) += 1.0;
    }
    for (int i = 0; i < SV.rows(); ++i)
      if (counts(i) > 0)
        p.col(i) /= counts(i);
    pose = p;
  }

  SimpleGeodesic::HeatGeodesicsData heat_data;
  if (!SimpleGeodesic::heat_geodesics_precompute(reassembled_data[0].transpose(), SF, heat_data))
  {
    ERROR_LOG_FMT(VIDEO, "Failed to precompute heat data!");
    return;
  }

  /**
   * @brief The Error Budget (Tolerance) for bone creation.
   *
   * How it works:
   * The algorithm splits the mesh into bones until the Total Squared Error (SSE)
   * of the reconstructed animation falls below this value.
   *
   * Range Guide (for a mesh scaled to 1.0 diagonal):
   * - 0.001 to 0.01: HIGH DETAIL. Produces 30-50 bones. (Fingers, facial features).
   * - 0.01 to 0.1: BALANCED. Produces 15-25 bones. (Standard game characters).
   * - 0.1 to 0.5: LOW DETAIL. Produces 5-10 bones. (LODs or background props).
   *
   * Note: If your mesh isn't scaled to 1.0, this value must be multiplied
   * by (scale^2) to remain effective.
   */
  // const double tolerance = 0.005;
  auto clean_grouping = GenerateRigFromMotion(reassembled_data, reassembled_data[0], heat_data);

  int bone_count = 0;
  for (int id : clean_grouping)
  {
    bone_count = std::max(bone_count, id + 1);
  }

  /*{
    const std::string path_prefix =
        File::GetUserPath(D_DUMP_IDX) + SConfig::GetInstance().GetGameID();

    const std::time_t start_time = std::time(nullptr);
    const auto local_time = Common::LocalTime(start_time);
    if (!local_time)
      return;

    const std::string base_name = fmt::format("{}_{:%Y-%m-%d_%H-%M-%S}", path_prefix, *local_time);

    const auto weights = ComputeHarmonicWeights(clean_grouping, reassembled_data[0], SF,
                                                reassembled_data, heat_data, bone_count);
    const auto influences = FindTopInfluences(weights);

    const auto bone_groups = ReconstructGroups(clean_grouping, bone_count);
    const auto bone_centers =
        ComputeBoneRestPositions(bone_groups, reassembled_data, reassembled_data[0]);
    ExportToGLTF(fmt::format("{}_bones_before_clean.gltf", base_name), reassembled_data[0], SF,
                 bone_centers, influences);
  }

  CleanBoneIslands(clean_grouping, SF, reassembled_data[0].cols(), bone_count);*/

  const std::string path_prefix =
      File::GetUserPath(D_DUMP_IDX) + SConfig::GetInstance().GetGameID();

  const std::time_t start_time = std::time(nullptr);
  const auto local_time = Common::LocalTime(start_time);
  if (!local_time)
    return;

  const std::string base_name = fmt::format("{}_{:%Y-%m-%d_%H-%M-%S}", path_prefix, *local_time);

  const auto weights = ComputeHarmonicWeights(clean_grouping, reassembled_data[0], SF,
                                              reassembled_data, heat_data, bone_count);
  const auto influences = FindTopInfluences(weights);

  const auto bone_groups = ReconstructGroups(clean_grouping, bone_count);
  const auto bone_centers =
      ComputeBoneRestPositions(bone_groups, reassembled_data, reassembled_data[0]);
  ExportToGLTF(fmt::format("{}_bones.gltf", base_name), SV.transpose(), SF, bone_centers,
               influences);

  File::IOFile outbound_file(fmt::format("{}.rig", base_name), "wb");
  ExportRigBinary(&outbound_file, reassembled_data[0], SVJ, influences, bone_centers,
                  m_draw_call_to_data, mesh_centroid, mesh_scale);
}
}  // namespace GraphicsModEditor
