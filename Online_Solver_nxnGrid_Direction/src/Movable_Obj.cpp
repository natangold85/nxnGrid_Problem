#include "Movable_Obj.h"


Movable_Obj::Movable_Obj(Coordinate& location, std::shared_ptr<Move_Properties> movement)
	: ObjInGrid(location)
	, m_movement(movement)
{
}