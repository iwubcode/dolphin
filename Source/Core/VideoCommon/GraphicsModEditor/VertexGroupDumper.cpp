// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "VideoCommon/GraphicsModEditor/VertexGroupDumper.h"

#include <random>
#include <utility>

#include "imgui/../../eigen/eigen/Eigen/Dense"
#include "imgui/../../eigen/eigen/Eigen/Geometry"

#include <fmt/chrono.h>
#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/JsonUtil.h"
#include "Common/TimeUtil.h"

#include "Core/ConfigManager.h"

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

Eigen::Matrix3Xf ConvertMeshPositions(const std::vector<Common::Vec3>& positions,
                                      const Common::Matrix44& transform_inverse)
{
  Eigen::Matrix3Xf result(3, positions.size());
  for (int i = 0; i < positions.size(); ++i)
  {
    const auto pos = positions[i];
    const auto pos_object_space = transform_inverse.Transform(pos, 1);
    result.col(i) = Eigen::Vector3f(pos_object_space.x, pos_object_space.y, pos_object_space.z);
  }

  return result;
}

bool IsDuplicateMesh(const Eigen::Matrix3Xf& mesh,
                     const std::vector<Eigen::Matrix3Xf>& existing_meshes, float epsilon = 1e-4f)
{
  for (const auto& existing_mesh : existing_meshes)
  {
    Eigen::Matrix3Xf diff = (mesh - existing_mesh).cwiseAbs();

    if (diff.maxCoeff() < epsilon)
    {
      return true;
    }
  }
  return false;
}

float DistanceSq(const Eigen::VectorXf& a, const Eigen::VectorXf& b)
{
  return (a - b).squaredNorm();
}

float CalculateWCSS(const Eigen::MatrixXf& descriptors, const std::vector<int>& assignments,
                    const std::vector<Eigen::VectorXf>& centroids, int B, int N)
{
  float wcss = 0.0f;
  for (int i = 0; i < N; ++i)
  {
    wcss += DistanceSq(descriptors.row(i), centroids[assignments[i]]);
  }
  return wcss;
}

// Modified K-Means function to return the WCSS (Inertia) value via output parameter
std::vector<int> BuildVertexGroups(const std::vector<Eigen::Matrix3Xf>& poses, int numGroups,
                                   float& out_wcss_score,
                                   const Eigen::MatrixXf& motionDescriptors_in)
{
  if (poses.empty())
  {
    return {};
  }

  const int N = motionDescriptors_in.rows();  // Number of vertices
  const std::size_t K = poses.size();         // Number of frames
  const int B = numGroups;                    // Number of bones/groups

  std::vector<int> assignments(N);  // Stores the final group ID for each vertex (0 to B-1)
  std::vector<Eigen::VectorXf> centroids(
      B,
      Eigen::VectorXf::Zero(3 * K));  // B centroids, each a 3*K length vector

  // --- 2. Centroid Initialization: Farthest-First Traversal ---

  // 2a. Start with one random centroid
  std::mt19937 rng(std::random_device{}());
  std::uniform_int_distribution<int> u_dist_func(0, N - 1);

  int firstIndex = u_dist_func(rng);
  centroids[0] = motionDescriptors_in.row(firstIndex);
  int numCentroidsInitialized = 1;

  // Track the distance from each point to its currently closest centroid
  std::vector<float> minDistances(N, std::numeric_limits<float>::max());

  while (numCentroidsInitialized < B)
  {
    float maxDist = -1.0f;
    int nextCentroidIndex = -1;

    // Update minimum distances for the most recently added centroid
    for (int i = 0; i < N; ++i)
    {
      float dist = DistanceSq(motionDescriptors_in.row(i), centroids[numCentroidsInitialized - 1]);
      if (dist < minDistances[i])
      {
        minDistances[i] = dist;
      }
    }

    // Find the data point that has the largest *minimum* distance (the farthest point)
    for (int i = 0; i < N; ++i)
    {
      if (minDistances[i] > maxDist)
      {
        maxDist = minDistances[i];
        nextCentroidIndex = i;
      }
    }

    // The farthest point becomes the next centroid
    centroids[numCentroidsInitialized] = motionDescriptors_in.row(nextCentroidIndex);
    numCentroidsInitialized++;
  }

  // --- 3. K-Means Iteration (Lloyd's Algorithm) ---

  const int maxIterations = 20;
  for (int iter = 0; iter < maxIterations; ++iter)
  {
    bool changed = false;

    // A. Assignment Step: Assign each vertex to the closest centroid
    for (int i = 0; i < N; ++i)
    {
      float minDist = std::numeric_limits<float>::max();
      int bestCluster = -1;
      for (int j = 0; j < B; ++j)
      {
        float dist = DistanceSq(motionDescriptors_in.row(i), centroids[j]);
        if (dist < minDist)
        {
          minDist = dist;
          bestCluster = j;
        }
      }
      if (assignments[i] != bestCluster)
      {
        assignments[i] = bestCluster;  // Assign to group j
        changed = true;
      }
    }

    // B. Update Step: Recalculate centroids as the mean of assigned vertices
    std::vector<int> counts(B, 0);
    for (int j = 0; j < B; ++j)
    {
      centroids[j].setZero();
    }

    for (int i = 0; i < N; ++i)
    {
      // Use += for Eigen vectors
      centroids[assignments[i]] += motionDescriptors_in.row(i);
      counts[assignments[i]]++;
    }

    for (int j = 0; j < B; ++j)
    {
      if (counts[j] > 0)
      {
        centroids[j] /= static_cast<float>(counts[j]);
      }
    }

    // Optional: Stop early if assignments didn't change
    if (!changed && iter > 0)
    {
      // std::cout << "K-Means converged after " << iter + 1 << " iterations." << std::endl;
      break;
    }
  }

  // Calculate and set the WCSS score
  out_wcss_score = CalculateWCSS(motionDescriptors_in, assignments, centroids, B, N);

  return assignments;
}

// Function to find the elbow point programmatically using the distance-to-line heuristic
int FindElbowPoint(const std::vector<int>& num_bones_options, const std::vector<float>& wcss_scores)
{
  int bestBValue = num_bones_options.back();
  float maxDistance = -1.0f;

  Eigen::Vector2f p1(static_cast<float>(num_bones_options.front()), wcss_scores.front());
  Eigen::Vector2f p2(static_cast<float>(num_bones_options.back()), wcss_scores.back());
  Eigen::Vector2f lineVec = p2 - p1;
  float lineLengthSq = lineVec.squaredNorm();

  if (lineLengthSq == 0.0f)
    return bestBValue;

  for (size_t i = 1; i < num_bones_options.size() - 1; ++i)
  {
    Eigen::Vector2f p_i(static_cast<float>(num_bones_options[i]), wcss_scores[i]);
    Eigen::Vector2f vec_to_pi = p_i - p1;
    float distance = std::abs(lineVec.x() * vec_to_pi.y() - lineVec.y() * vec_to_pi.x()) /
                     std::sqrt(lineLengthSq);

    if (distance > maxDistance)
    {
      maxDistance = distance;
      bestBValue = num_bones_options[i];
    }
  }
  return bestBValue;
}

std::vector<int> CalculateOptimalGrouping(const std::vector<Eigen::Matrix3Xf>& capturedPoses)
{
  if (capturedPoses.empty())
    return {};

  // 1. Pre-calculate the motion descriptors once
  const int N = capturedPoses.front().cols();
  const std::size_t K = capturedPoses.size();
  Eigen::MatrixXf motionDescriptors(N, 3 * K);
  for (int k = 0; k < K; ++k)
  {
    // Ensure every pose has the correct number of vertices
    if (capturedPoses[k].cols() != N)
    {
      return {};
    }
    // Use a temporary transpose to convert 3xN data into N rows of 3 columns, then evaluate
    motionDescriptors.block(0, k * 3, N, 3) = capturedPoses[k].transpose().eval();
  }

  // 2. Define range of bones to test
  std::vector<int> num_bones_options;
  for (int b = 5; b <= 50; b += 5)
  {  // Test B=5, 10, 15, ..., 50
    num_bones_options.push_back(b);
  }
  std::vector<float> wcss_scores;

  // 3. Run K-Means for each option and store WCSS
  for (int B_option : num_bones_options)
  {
    float currentWCSS = 0.0f;
    // The assignment result is temporary here, only the WCSS score is needed for the elbow method
    BuildVertexGroups(capturedPoses, B_option, currentWCSS, motionDescriptors);
    wcss_scores.push_back(currentWCSS);
  }

  // 4. Find the optimal number of groups using the elbow heuristic
  int optimalNumGroups = FindElbowPoint(num_bones_options, wcss_scores);

  // 5. Run K-Means one final time with the optimal number
  float finalWCSS;
  std::vector<int> finalOptimalGroups =
      BuildVertexGroups(capturedPoses, optimalNumGroups, finalWCSS, motionDescriptors);

  return finalOptimalGroups;
}

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

  Common::Matrix44 position_transform;
  for (std::size_t i = 0; i < 3; i++)
  {
    position_transform.data[i * 4 + 0] = draw_data.object_transform[i][0];
    position_transform.data[i * 4 + 1] = draw_data.object_transform[i][1];
    position_transform.data[i * 4 + 2] = draw_data.object_transform[i][2];
    position_transform.data[i * 4 + 3] = draw_data.object_transform[i][3];
  }
  position_transform.data[12] = 0;
  position_transform.data[13] = 0;
  position_transform.data[14] = 0;
  position_transform.data[15] = 1;

  Eigen::Matrix3Xf matrix_positions =
      ConvertMeshPositions(current_vertex_positions, position_transform.Inverted());
  auto& data = m_draw_call_to_data[draw_call_id];

  if (IsDuplicateMesh(matrix_positions, data.vertex_meshes))
    return;

  data.vertex_meshes.push_back(std::move(matrix_positions));
}

void VertexGroupDumper::Record(const VertexGroupRecordingRequest& request)
{
  m_recording_request = request;
}

void VertexGroupDumper::StopRecord()
{
  m_recording_request.reset();

  picojson::object json_data;
  for (const auto& [draw_call, data] : m_draw_call_to_data)
  {
    picojson::object group_data;

    const auto grouping = CalculateOptimalGrouping(data.vertex_meshes);
    for (std::size_t i = 0; i < grouping.size(); i++)
    {
      const auto group_assignment = grouping[i];
      const auto [iter, added] =
          group_data.try_emplace(std::to_string(group_assignment), picojson::array{});
      iter->second.get<picojson::array>().emplace_back(static_cast<double>(i));
    }

    json_data.try_emplace(std::to_string(std::to_underlying(draw_call)), group_data);
  }
  m_draw_call_to_data.clear();

  const std::string path_prefix =
      File::GetUserPath(D_DUMP_IDX) + SConfig::GetInstance().GetGameID();

  const std::time_t start_time = std::time(nullptr);
  const auto local_time = Common::LocalTime(start_time);
  if (!local_time)
    return;

  const std::string base_name = fmt::format("{}_{:%Y-%m-%d_%H-%M-%S}", path_prefix, *local_time);
  const std::string save_path = fmt::format("{}.vertexgroups", base_name);

  if (!JsonToFile(save_path, picojson::value{json_data}, true))
  {
    // TODO: error
  }
}

bool VertexGroupDumper::IsRecording() const
{
  return m_recording_request.has_value();
}
}  // namespace GraphicsModEditor
