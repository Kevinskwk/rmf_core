/*
 * Copyright (C) 2020 Open Source Robotics Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
*/

#include <rmf_fleet_adapter/agv/parse_graph.hpp>
#include <iostream>
#include <unordered_map>
#include <yaml-cpp/yaml.h>

namespace rmf_fleet_adapter {
namespace agv {

//==============================================================================
rmf_traffic::agv::Graph parse_graph(
  const std::string& graph_file,
  const rmf_traffic::agv::VehicleTraits& vehicle_traits)
{
  const YAML::Node graph_config = YAML::LoadFile(graph_file);
  if (!graph_config)
  {
    throw std::runtime_error("Failed to load graph file [" + graph_file + "]");
  }

  const YAML::Node levels = graph_config["levels"];
  if (!levels)
  {
    throw std::runtime_error(
          "Graph file [" + graph_file + "] is missing the [levels] key");
  }

  if (!levels.IsMap())
  {
    throw std::runtime_error(
          "The [levels] key does not point to a map in graph file ["
          + graph_file + "]");
  }

  rmf_traffic::agv::Graph graph;
  std::unordered_map<std::string, rmf_fleet_adapter::agv::LiftWps> lift_wps;
  std::size_t vnum = 0;  // To increment lane endpoint ids

  using Constraint = rmf_traffic::agv::Graph::OrientationConstraint;
  using ConstraintPtr = rmf_utils::clone_ptr<Constraint>;

  using Lane = rmf_traffic::agv::Graph::Lane;
  using Event = Lane::Event;

  for (const auto& level : levels)
  {
    const std::string& map_name = level.first.as<std::string>();
    std::size_t vnum_temp = 0;

    const YAML::Node& vertices = level.second["vertices"];
    for (const auto& vertex : vertices)
    {
      const Eigen::Vector2d location{
        vertex[0].as<double>(), vertex[1].as<double>()};

      auto& wp = graph.add_waypoint(map_name, location);

      const YAML::Node& options = vertex[2];
      const YAML::Node& name_option = options["name"];
      if (name_option)
      {
        const std::string& name = name_option.as<std::string>();
        if (!name.empty())
        {
          if (!graph.add_key(name, wp.index()))
          {
            throw std::runtime_error(
              "Duplicated waypoint name [" + name + "] in graph ["
                  + graph_file + "]");
          }
        }
      }
      vnum_temp ++;

      const YAML::Node& parking_spot_option = options["is_parking_spot"];
      if (parking_spot_option)
      {
        const bool is_parking_spot = parking_spot_option.as<bool>();
        if (is_parking_spot)
          wp.set_parking_spot(true);
      }

      const YAML::Node& lift_option = options["lift"];
      if (lift_option)
      {
        const std::string lift_name = lift_option.as<std::string>();
        //std::cout << lift_name << " " << wp.index() << std::endl;
        if (lift_name != "")
          lift_wps[lift_name].cabin_wps.insert(wp.index());
      }
    }

    const YAML::Node& lanes = level.second["lanes"];
    for (const auto& lane : lanes)
    {
      ConstraintPtr constraint = nullptr;

      const YAML::Node& options = lane[2];
      const YAML::Node& orientation_constraint_option =
        options["orientation_constraint"];
      if (orientation_constraint_option)
      {
        const std::string& constraint_label =
          orientation_constraint_option.as<std::string>();
        if (constraint_label == "forward")
        {
          constraint = Constraint::make(
            Constraint::Direction::Forward,
            vehicle_traits.get_differential()->get_forward());
        }
        else if (constraint_label == "backward")
        {
          constraint = Constraint::make(
            Constraint::Direction::Backward,
            vehicle_traits.get_differential()->get_forward());
        }
        else
        {
          throw std::runtime_error(
            "Unrecognized orientation constraint label given to lane ["
            + std::to_string(lane[0].as<std::size_t>() + vnum) + ", "
            + std::to_string(lane[1].as<std::size_t>() + vnum) + "]: ["
            + constraint_label + "] in graph ["
            + graph_file + "]");
        }
      }

      rmf_utils::clone_ptr<Event> entry_event;
      rmf_utils::clone_ptr<Event> exit_event;
      /*if (const YAML::Node mock_lift_option = options["demo_mock_floor_name"])
      {
        // TODO(MXG): Replace this with a key like lift_name when we have proper
        // support for lifts.
        const std::string floor_name = mock_lift_option.as<std::string>();
        const YAML::Node lift_name_option = options["demo_mock_lift_name"];

        if (!lift_name_option)
        {
          throw std::runtime_error(
            "Missing [demo_mock_lift_name] parameter which is required for "
            "mock lifts");
        }

        const std::string lift_name = lift_name_option.as<std::string>();
        const rmf_traffic::Duration duration = std::chrono::seconds(4);
        entry_event = Event::make(
          Lane::LiftDoorOpen(lift_name, floor_name, duration));
        // NOTE(MXG): We do not need an exit event for lifts
      }
      else */

      if (const YAML::Node door_name_option = options["door_name"])
      {
        const std::string name = door_name_option.as<std::string>();
        const rmf_traffic::Duration duration = std::chrono::seconds(4);
        entry_event = Event::make(Lane::DoorOpen(name, duration));
        exit_event = Event::make(Lane::DoorClose(name, duration));
      }

      if (const YAML::Node docking_option = options["dock_name"])
      {
        // TODO(MXG): Add support for this
        if (entry_event || exit_event)
        {
          // *INDENT-OFF*
          throw std::runtime_error(
            "We do not currently support a dock_name option when any other "
            "lane options are also specified");
          // *INDENT-ON*
        }

        const std::string dock_name = docking_option.as<std::string>();
        const rmf_traffic::Duration duration = std::chrono::seconds(5);
        entry_event = Event::make(Lane::Dock(dock_name, duration));
      }

      std::size_t begin = lane[0].as<std::size_t>() + vnum;
      std::size_t end = lane[1].as<std::size_t>() + vnum;

      bool add_lane = true;

      for (auto& lift : lift_wps)
      {
        auto wps = lift.second.cabin_wps;
        if (std::find(wps.begin(), wps.end(), begin) != wps.end())
        {
          lift.second.entry_wps.insert(end);
          add_lane = false;
          break;
        }
        if (std::find(wps.begin(), wps.end(), end) != wps.end())
        {
          lift.second.entry_wps.insert(begin);
          add_lane = false;
          break;
        }
      }

      if (add_lane)
      {
        graph.add_lane(
          {lane[0].as<std::size_t>() + vnum, entry_event},
          {lane[1].as<std::size_t>() + vnum, exit_event, std::move(constraint)});
        //std::cout << lane[0].as<std::size_t>() << "," << lane[1].as<std::size_t>() << std::endl;
      }
    }
    vnum += vnum_temp;
  }

  for (const auto& lift : lift_wps)
  {
    for (const auto& cabin_id : lift.second.cabin_wps)
    {
      for (const auto& entry_id : lift.second.entry_wps)
      {
        auto cabin_wp = graph.get_waypoint(cabin_id);
        auto entry_wp = graph.get_waypoint(entry_id);

        ConstraintPtr constraint = nullptr;
        rmf_utils::clone_ptr<Event> entry_event;
        rmf_utils::clone_ptr<Event> exit_event;
        const rmf_traffic::Duration duration = std::chrono::seconds(4);
        entry_event = Event::make(
          Lane::LiftDoorOpen(lift.first, entry_wp.get_map_name(), duration));

        if (cabin_wp.get_map_name() != entry_wp.get_map_name())
        {
          graph.add_lane(
            {cabin_id, entry_event},
            {entry_id, exit_event, std::move(constraint)});
        }
        else
        {
          graph.add_lane(
            {entry_id, entry_event},
            {cabin_id, exit_event, std::move(constraint)});  
        }
        //std::cout << entry_id << "," << cabin_id << std::endl;
      }
    }
  }

  return graph;
}

} // namespace agv
} // namespace rmf_fleet_adapter
