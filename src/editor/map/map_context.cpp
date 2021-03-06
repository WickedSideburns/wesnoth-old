/*
   Copyright (C) 2008 - 2013 by Tomasz Sniatowski <kailoran@gmail.com>
   Part of the Battle for Wesnoth Project http://www.wesnoth.org/

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/
#define GETTEXT_DOMAIN "wesnoth-editor"

#include "editor/action/action.hpp"
#include "map_context.hpp"

#include "display.hpp"
#include "filesystem.hpp"
#include "gettext.hpp"
#include "map_exception.hpp"
#include "map_label.hpp"
#include "serialization/binary_or_text.hpp"
#include "serialization/parser.hpp"
#include "team.hpp"
#include "wml_exception.hpp"


#include "formula_string_utils.hpp"

#include <boost/regex.hpp>
#include <boost/foreach.hpp>


namespace editor {

const size_t map_context::max_action_stack_size_ = 100;

map_context::map_context(const editor_map& map, const display& disp)
	: filename_()
	, map_data_key_()
	, embedded_(false)
	, map_(map)
	, undo_stack_()
	, redo_stack_()
	, actions_since_save_(0)
	, starting_position_label_locs_()
	, needs_reload_(false)
	, needs_terrain_rebuild_(false)
	, needs_labels_reset_(false)
	, changed_locations_()
	, everything_changed_(false)
	, active_area_(1)
	, labels_(disp, NULL)
	, units_()
	, teams_()
	, tod_manager_(new tod_manager)
	, state_()
{
}

map_context::map_context(const config& game_config, const std::string& filename, const display& disp)
	: filename_(filename)
	, map_data_key_()
	, embedded_(false)
	, map_(game_config)
	, undo_stack_()
	, redo_stack_()
	, actions_since_save_(0)
	, starting_position_label_locs_()
	, needs_reload_(false)
	, needs_terrain_rebuild_(false)
	, needs_labels_reset_(false)
	, changed_locations_()
	, everything_changed_(false)
	, active_area_(0)
	, labels_(disp, NULL)
	, units_()
	, teams_()
	, tod_manager_(NULL)
	, state_()
{
	/*
	 * Overview of situations possibly found in the file:
	 *
	 * 0. Not a scenario or map file.
	 *    0.1 File not found
	 *    0.2 Map file empty
	 *    0.3
	 * 1. It's a file containing only pure map data.
	 *    * embedded_ = false
	 *    *
	 * 2. A scenario embedding the map
	 *    The data/scenario-test.cfg for example.
	 *    The map is written back to the file.
	 *
	 * 3. The file contains a [map]
	 *
	 */

	log_scope2(log_editor, "Loading map " + filename);
	// Case 0.1
	if (!file_exists(filename) || is_directory(filename)) {
		throw editor_map_load_exception(filename, _("File not found"));
	}
	std::string map_string = read_file(filename);
	boost::regex re("map_data\\s*=\\s*\"(.+?)\"");
	boost::smatch m;
	if (boost::regex_search(map_string, m, re, boost::regex_constants::match_not_dot_null)) {
		boost::regex re2("\\{(.+?)\\}");
		boost::smatch m2;
		std::string m1 = m[1];
		if (boost::regex_search(m1, m2, re2)) {
			map_data_key_ = m1;
			LOG_ED << "Map looks like a scenario, trying {" << m2[1] << "}\n";
			std::string new_filename = get_wml_location(m2[1], directory_name(m2[1]));
			if (new_filename.empty()) {
				std::string message = _("The map file looks like a scenario, "
					"but the map_data value does not point to an existing file")
					+ std::string("\n") + m2[1];
				throw editor_map_load_exception(filename, message);
			}
			LOG_ED << "New filename is: " << new_filename << "\n";
			filename_ = new_filename;
			map_string = read_file(filename_);
		} else {
			LOG_ED << "Loading embedded map file\n";
			embedded_ = true;
			map_string = m[1];
		}
	}
	// Case 0.2
	if (map_string.empty()) {
		std::string message = _("Empty map file");
		throw editor_map_load_exception(filename, message);
	}
	map_ = editor_map::from_string(game_config, map_string); //throws on error

	config level;
	read(level, *(preprocess_file(filename_ + ".cfg")));

	labels_.read(level);

	tod_manager_.reset(new tod_manager(level));
	BOOST_FOREACH(const config &t, level.child_range("time_area")) {
		tod_manager_->add_time_area(t);
	}

	resources::teams = &teams_;

	int i = 1;
	BOOST_FOREACH(config &side, level.child_range("side"))
	{
		//TODO clean up.
		//state_.build_team(side, "", teams_, level, *this
		//	, units_, false);
		team t;
		side["side"] = i;
		//side["no_leader"] = "yes";
		i++;
		t.build(side, map_);

		teams_.push_back(t);
		BOOST_FOREACH(const config &a_unit, side.child_range("unit")) {
			map_location loc(a_unit, NULL);
			units_.add(loc,
					unit(a_unit, true, &state_) );
		}
	}

}

map_context::~map_context()
{
	clear_stack(undo_stack_);
	clear_stack(redo_stack_);
}

bool map_context::select_area(int index)
{
	return map_.set_selection(tod_manager_->get_area_by_index(index));
}

void map_context::draw_terrain(const t_translation::t_terrain & terrain,
	const map_location& loc, bool one_layer_only)
{
	t_translation::t_terrain full_terrain = one_layer_only ? terrain :
		map_.get_terrain_info(terrain).terrain_with_default_base();

	draw_terrain_actual(full_terrain, loc, one_layer_only);
}

void map_context::draw_terrain_actual(const t_translation::t_terrain & terrain,
	const map_location& loc, bool one_layer_only)
{
	if (!map_.on_board_with_border(loc)) {
		//requests for painting off the map are ignored in set_terrain anyway,
		//but ideally we should not have any
		LOG_ED << "Attempted to draw terrain off the map (" << loc << ")\n";
		return;
	}
	t_translation::t_terrain old_terrain = map_.get_terrain(loc);
	if (terrain != old_terrain) {
		if (terrain.base == t_translation::NO_LAYER) {
			map_.set_terrain(loc, terrain, gamemap::OVERLAY);
		} else if (one_layer_only) {
			map_.set_terrain(loc, terrain, gamemap::BASE);
		} else {
			map_.set_terrain(loc, terrain);
		}
		add_changed_location(loc);
	}
}

void map_context::draw_terrain(const t_translation::t_terrain & terrain,
	const std::set<map_location>& locs, bool one_layer_only)
{
	t_translation::t_terrain full_terrain = one_layer_only ? terrain :
		map_.get_terrain_info(terrain).terrain_with_default_base();

	BOOST_FOREACH(const map_location& loc, locs) {
		draw_terrain_actual(full_terrain, loc, one_layer_only);
	}
}

void map_context::clear_changed_locations()
{
	everything_changed_ = false;
	changed_locations_.clear();
}

void map_context::add_changed_location(const map_location& loc)
{
	if (!everything_changed()) {
		changed_locations_.insert(loc);
	}
}

void map_context::add_changed_location(const std::set<map_location>& locs)
{
	if (!everything_changed()) {
		changed_locations_.insert(locs.begin(), locs.end());
	}
}

void map_context::set_everything_changed()
{
	everything_changed_ = true;
}

bool map_context::everything_changed() const
{
	return everything_changed_;
}

void map_context::clear_starting_position_labels(display& disp)
{
	disp.labels().clear_all();
	starting_position_label_locs_.clear();
}

void map_context::set_starting_position_labels(display& disp)
{
	std::set<map_location> new_label_locs = map_.set_starting_position_labels(disp);
	starting_position_label_locs_.insert(new_label_locs.begin(), new_label_locs.end());
}

void map_context::reset_starting_position_labels(display& disp)
{
	clear_starting_position_labels(disp);
	set_starting_position_labels(disp);
	set_needs_labels_reset(false);
}

bool map_context::save()
{
	std::string data = map_.write();

	config wml_data = tod_manager_->to_config();
	labels_.write(wml_data);

	//TODO think about saving the map to the wml file
	//config& map = cfg.add_child("map");
	//gamemap::write(map);

	std::stringstream buf;

	for(std::vector<team>::const_iterator t = teams_.begin(); t != teams_.end(); ++t) {
		int side_num = t - teams_.begin() + 1;

		config& side = wml_data.add_child("side");
		t->write(side);
		// TODO make this customizable via gui
		side["no_leader"] = "no";
		side["allow_player"] = "yes";
		side.remove_attribute("color");
		side.remove_attribute("recruit");
		side.remove_attribute("recall_cost");
		side.remove_attribute("gold");
		side.remove_attribute("start_gold");
		side.remove_attribute("hidden");
		buf.str(std::string());
		buf << side_num;
		side["side"] = buf.str();

		//current visible units
		for(unit_map::const_iterator i = units_.begin(); i != units_.end(); ++i) {
			if(i->side() == side_num) {
				config& u = side.add_child("unit");
				i->get_location().write(u); // TODO: Needed?
				i->write(u);
			}
		}
	}

	try {
		if (!is_embedded()) {
			write_file(get_filename(), data);

			std::stringstream wml_stream;
			{
				config_writer out(wml_stream, false);
				out.write(wml_data);
			}
			if (!wml_stream.str().empty()) {
				write_file(get_filename() + ".cfg", wml_stream.str());
			}
		} else {
			std::string map_string = read_file(get_filename());
			boost::regex re("(.*map_data\\s*=\\s*\")(.+?)(\".*)");
			boost::smatch m;
			if (boost::regex_search(map_string, m, re, boost::regex_constants::match_not_dot_null)) {
				std::stringstream ss;
				ss << m[1];
				ss << data;
				ss << m[3];
				write_file(get_filename(), ss.str());
			} else {
				throw editor_map_save_exception(_("Could not save into scenario"));
			}
		}
		clear_modified();
	} catch (io_exception& e) {
		utils::string_map symbols;
		symbols["msg"] = e.what();
		const std::string msg = vgettext("Could not save the map: $msg", symbols);
		throw editor_map_save_exception(msg);
	}
	//TODO the return value of this method does not need to be boolean.
	//We either return true or there is an exception thrown.
	return true;
}

void map_context::set_map(const editor_map& map)
{
	if (map_.h() != map.h() || map_.w() != map.w()) {
		set_needs_reload();
	} else {
		set_needs_terrain_rebuild();
	}
	map_ = map;
}

void map_context::perform_action(const editor_action& action)
{
	LOG_ED << "Performing action " << action.get_id() << ": " << action.get_name()
		<< ", actions count is " << action.get_instance_count() << "\n";
	editor_action* undo = action.perform(*this);
	if (actions_since_save_ < 0) {
		//set to a value that will make it impossible to get to zero, as at this point
		//it is no longer possible to get back the original map state using undo/redo
		actions_since_save_ = 1 + undo_stack_.size();
	}
	actions_since_save_++;
	undo_stack_.push_back(undo);
	trim_stack(undo_stack_);
	clear_stack(redo_stack_);
}

void map_context::perform_partial_action(const editor_action& action)
{
	LOG_ED << "Performing (partial) action " << action.get_id() << ": " << action.get_name()
		<< ", actions count is " << action.get_instance_count() << "\n";
	if (!can_undo()) {
		throw editor_logic_exception("Empty undo stack in perform_partial_action()");
	}
	editor_action_chain* undo_chain = dynamic_cast<editor_action_chain*>(last_undo_action());
	if (undo_chain == NULL) {
		throw editor_logic_exception("Last undo action not a chain in perform_partial_action()");
	}
	editor_action* undo = action.perform(*this);
	//actions_since_save_ += action.action_count();
	undo_chain->prepend_action(undo);
	clear_stack(redo_stack_);
}
bool map_context::modified() const
{
	return actions_since_save_ != 0;
}

void map_context::clear_modified()
{
	actions_since_save_ = 0;
}

bool map_context::can_undo() const
{
	return !undo_stack_.empty();
}

bool map_context::can_redo() const
{
	return !redo_stack_.empty();
}

editor_action* map_context::last_undo_action()
{
	return undo_stack_.empty() ? NULL : undo_stack_.back();
}

editor_action* map_context::last_redo_action()
{
	return redo_stack_.empty() ? NULL : redo_stack_.back();
}

const editor_action* map_context::last_undo_action() const
{
	return undo_stack_.empty() ? NULL : undo_stack_.back();
}

const editor_action* map_context::last_redo_action() const
{
	return redo_stack_.empty() ? NULL : redo_stack_.back();
}

void map_context::undo()
{
	LOG_ED << "undo() beg, undo stack is " << undo_stack_.size() << ", redo stack " << redo_stack_.size() << "\n";
	if (can_undo()) {
		perform_action_between_stacks(undo_stack_, redo_stack_);
		actions_since_save_--;
	} else {
		WRN_ED << "undo() called with an empty undo stack\n";
	}
	LOG_ED << "undo() end, undo stack is " << undo_stack_.size() << ", redo stack " << redo_stack_.size() << "\n";
}

void map_context::redo()
{
	LOG_ED << "redo() beg, undo stack is " << undo_stack_.size() << ", redo stack " << redo_stack_.size() << "\n";
	if (can_redo()) {
		perform_action_between_stacks(redo_stack_, undo_stack_);
		actions_since_save_++;
	} else {
		WRN_ED << "redo() called with an empty redo stack\n";
	}
	LOG_ED << "redo() end, undo stack is " << undo_stack_.size() << ", redo stack " << redo_stack_.size() << "\n";
}

void map_context::partial_undo()
{
	//callers should check for these conditions
	if (!can_undo()) {
		throw editor_logic_exception("Empty undo stack in partial_undo()");
	}
	editor_action_chain* undo_chain = dynamic_cast<editor_action_chain*>(last_undo_action());
	if (undo_chain == NULL) {
		throw editor_logic_exception("Last undo action not a chain in partial undo");
	}
	//a partial undo performs the first action form the current action's action_chain that would be normally performed
	//i.e. the *first* one.
	boost::scoped_ptr<editor_action> first_action_in_chain(undo_chain->pop_first_action());
	if (undo_chain->empty()) {
		actions_since_save_--;
		delete undo_chain;
		undo_stack_.pop_back();
	}
	redo_stack_.push_back(first_action_in_chain.get()->perform(*this));
	//actions_since_save_ -= last_redo_action()->action_count();
}

void map_context::clear_undo_redo()
{
	clear_stack(undo_stack_);
	clear_stack(redo_stack_);
}

void map_context::trim_stack(action_stack& stack)
{
	if (stack.size() > max_action_stack_size_) {
		delete stack.front();
		stack.pop_front();
	}
}

void map_context::clear_stack(action_stack& stack)
{
	BOOST_FOREACH(editor_action* a, stack) {
		delete a;
	}
	stack.clear();
}

void map_context::perform_action_between_stacks(action_stack& from, action_stack& to)
{
	assert(!from.empty());
	boost::scoped_ptr<editor_action> action(from.back());
	from.pop_back();
	editor_action* reverse_action = action->perform(*this);
	to.push_back(reverse_action);
	trim_stack(to);
}

} //end namespace editor
