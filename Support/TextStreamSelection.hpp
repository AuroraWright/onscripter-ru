/**
 *  TextStreamSelection.hpp
 *  ONScripter-RU
 *
 *  Scope script annotations to a single command, including nested execution.
 *
 *  Consult LICENSE file for licensing terms and copyright holders.
 */

#pragma once

#include <cstring>
#include <set>

class TextStreamSelection {
	std::set<size_t> pending;
	bool active{false};
	bool spriteOnly{false};
	unsigned int generation{0};

public:
	void mark(size_t context = 0) { pending.insert(context); }
	void clear() { pending.clear(); active = false; ++generation; }
	bool requested() const { return active; }
	bool consume(bool sprite = true) {
		if (spriteOnly && !sprite) return false;
		bool result = active;
		active = false;
		return result;
	}

	class Scope {
		TextStreamSelection *selection;
		bool previous;
		bool previousSpriteOnly;
		unsigned int generation;

	public:
		Scope(TextStreamSelection &owner, const char *command, size_t context = 0)
		    : selection(&owner), previous(owner.active), previousSpriteOnly(owner.spriteOnly), generation(owner.generation) {
			owner.active = false;
			if (*command == '_') ++command;
			owner.spriteOnly = std::strncmp(command, "lsp", 3) == 0;
			if (*command && *command != ';' && *command != '*' && *command != ':' && *command != '\n') {
				owner.active = owner.pending.erase(context) != 0;
			}
		}
		Scope(const Scope &) = delete;
		Scope &operator=(const Scope &) = delete;
		Scope(Scope &&other)
		    : selection(other.selection), previous(other.previous), previousSpriteOnly(other.previousSpriteOnly), generation(other.generation) {
			other.selection = nullptr;
		}
		~Scope() {
			if (selection) {
				selection->active = selection->generation == generation ? previous : false;
				selection->spriteOnly = previousSpriteOnly;
			}
		}
	};
};
