#include <string>
#include <math.h>
#include <limits.h>

#include "..\include\despot\util\random.h"
#include "..\include\despot\solver\pomcp.h"

#include "nxnGridGlobalActions.h"
#include "Coordinate.h"

namespace despot 
{
/// changes surrounding a point
static const int s_numMoves = 8;
static const int s_lutDirections[s_numMoves][2] = { { 0, 1 }, { 0, -1 }, { 1, 0 }, { -1, 0 }, { 1, 1 }, { 1, -1 }, { -1, 1 }, { -1, -1 } };

static bool s_IS_MOVE_FROM_ENEMY = true;
/// string array for all actions (enum as idx)
static std::vector<std::string> s_ACTION_STR;

/// observation for loss and win (does not important)
static int OB_LOSS = 0;
static int OB_WIN = 0;

/* =============================================================================
* inline Functions
* =============================================================================*/

/// return absolute value of a number
inline int Abs(int num)
{
	return num * (num >= 0) - num * (num < 0);
}

/// return min(a,b)
inline int Min(int a, int b)
{
	return a * (a <= b) + b * (b < a);
}

/// return min(a,b)
inline int Max(int a, int b)
{
	return a * (a >= b) + b * (b > a);
}

/// return min(a,b)
inline int Distance(int a, int b, int gridSize)
{
	int xDiff = a % gridSize - b % gridSize;
	int yDiff = a / gridSize - b / gridSize;
	return xDiff * xDiff + yDiff * yDiff;
}


/* =============================================================================
* nxnGridGlobalActions Functions
* =============================================================================*/

nxnGridGlobalActions::nxnGridGlobalActions(int gridSize, int target, Self_Obj & self, std::vector<intVec> & objectsInitLoc, bool isMoveFromEnemyExist)
: nxnGrid(gridSize, target, self, objectsInitLoc)
{
	// init vector for string of actions
	s_ACTION_STR.clear();
	s_ACTION_STR.emplace_back("Move To Target");
	s_numBasicActions = NumBasicActions();
	s_numEnemyRelatedActions = 1 + isMoveFromEnemyExist;
	s_IS_MOVE_FROM_ENEMY = isMoveFromEnemyExist;
}

bool nxnGridGlobalActions::Step(State& s, double randomSelfAction, int action, OBS_TYPE lastObs, double& reward, OBS_TYPE& obs) const
{
	intVec state;
	nxnGridState::IdxToState(&s, state);

	// drawing more random numbers for each variable
	double randomSelfObservation = Random::RANDOM.NextDouble();

	std::vector<double> randomObjectMoves;
	CreateRandomVec(randomObjectMoves, CountMovingObjects() - 1);
	
	std::vector<double> randomEnemiesAttacks;
	CreateRandomVec(randomEnemiesAttacks, m_enemyVec.size());

	reward = REWARD_STEP;

	// run on all enemies and check if the robot was killed
	for (int i = 0; i < m_enemyVec.size(); ++i)
	{
		if (state[i + 1] != m_gridSize * m_gridSize && CalcIfDead(i, state, randomEnemiesAttacks[i]))
		{
			reward = REWARD_LOSS;
			return true;
		}
	}

	// if we are at the target end game with a win
	if (m_targetIdx == state[0])
	{	
		reward = REWARD_WIN;
		return true;
	}

	// run on actions
	if (action == MOVE_TO_TARGET)
	{
		MoveToTarget(state, randomSelfAction);
	}
	else if(action == MOVE_TO_SHELTER & m_shelters.size() > 0)
	{
		MoveToShelter(state, randomSelfAction);
	}
	else // actions related to enemies
	{
		int enemyAction = (action - NumBasicActions()) % NumEnemyActions();
		int enemyIdx = (action - NumBasicActions()) / NumEnemyActions();
		++enemyIdx;

		intVec observedState;
		nxnGridState::IdxToState(lastObs, observedState);
		int obsEnemyLoc = observedState[enemyIdx];

		if (state[enemyIdx] == m_gridSize * m_gridSize | obsEnemyLoc == observedState[0])
		{
				reward += REWARD_ILLEGAL_MOVE;
		}	
		else if (enemyAction == ATTACK)
		{
			Attack(state, obsEnemyLoc, randomSelfAction, reward);
			for (size_t i = 0; i < m_nonInvolvedVec.size(); ++i)
			{
				if (state[i + 1 + m_enemyVec.size()] == m_gridSize * m_gridSize)
				{
					reward = REWARD_KILL_NINV;
					return true;
				}
			}
			reward += REWARD_KILL_ENEMY * (state[enemyIdx] == m_gridSize * m_gridSize);
		}
		else // action = movefromenemy
			MoveFromEnemy(state, obsEnemyLoc, randomSelfAction, lastObs);

	}

	// set next position of the objects on grid
	SetNextPosition(state, randomObjectMoves);
	// update observation
	obs = FindObservation(state, randomSelfObservation);
	
	//update state
	nxnGridState& stateClass = static_cast<nxnGridState&>(s);
	stateClass.UpdateState(state);
	return false;
}

int nxnGridGlobalActions::NumBasicActions() const
{
	return 1 + (m_shelters.size() > 0);
}

int nxnGridGlobalActions::NumEnemyActions() const
{
	return 1 + s_IS_MOVE_FROM_ENEMY;
}

int nxnGridGlobalActions::NumActions() const
{
	// for any enemy add attack and move from enemy

	return NumBasicActions() + m_enemyVec.size() * NumEnemyActions();
};

ValuedAction nxnGridGlobalActions::GetMinRewardAction() const
{
	return ValuedAction(rand() % NumActions(), -10.0);
}

void nxnGridGlobalActions::PrintAction(int action, std::ostream & out) const
{
	out << s_ACTION_STR[action] << std::endl;
}

void nxnGridGlobalActions::AddActionsToEnemy()
{
	int enemyNum = m_enemyVec.size();
	s_ACTION_STR.push_back("Attack enemy #" + std::to_string(enemyNum));
	if (s_IS_MOVE_FROM_ENEMY)
		s_ACTION_STR.push_back("Move from enemy #" + std::to_string(enemyNum));
}

void nxnGridGlobalActions::AddActionsToShelter()
{
	// insert move to shelter after first action (move to target)
	if (m_shelters.size() == 1)
		s_ACTION_STR.insert(s_ACTION_STR.begin() + 1, "Move to shelter");

	s_numBasicActions = NumBasicActions();
}

void nxnGridGlobalActions::MoveToTarget(intVec & state, double random) const
{
	MoveToLocation(state, m_targetIdx, random);
}

void nxnGridGlobalActions::MoveToShelter(intVec & state, double random) const
{
	// go to the nearest shelter
	int shelterLoc = NearestShelter(state[0]);
	
	if (shelterLoc != state[0])
		MoveToLocation(state, shelterLoc, random);
}

void nxnGridGlobalActions::Attack(intVec & state, int target, double random, double & reward) const
{

	if (m_self.GetAttack()->InRange(state[0], target, m_gridSize))
	{
		intVec shelters(m_shelters.size());
		for (size_t i = 0; i < m_shelters.size(); ++i)
			shelters[i] = m_shelters[i].GetLocation().GetIdx(m_gridSize);

		m_self.AttackOnline(state[0], target, state, shelters, m_gridSize, random);
		reward += REWARD_FIRE;
	}
	else
		MoveToLocation(state, target, random);
}

void nxnGridGlobalActions::MoveFromEnemy(intVec & state, int obsEnemyLoc, double random, OBS_TYPE lastObs) const
{
	Coordinate enemy(obsEnemyLoc % m_gridSize, obsEnemyLoc / m_gridSize);
	int newLoc = MoveFromLocation(state, enemy);

	if (newLoc == state[0])
		return;

	std::map<int, double> possibleLocations;
	m_self.GetMovement()->GetPossibleMoves(state[0], m_gridSize, state, possibleLocations, newLoc);

	int loc = -1;
	for (auto v : possibleLocations)
	{
		loc = v.first;
		random -= v.second;
		if (random <= 0.0)
			break;
	}

	state[0] = loc;
}

void nxnGridGlobalActions::MoveToLocation(intVec & state, int location, double random) const
{
	std::map<int, double> possibleLocations;

	m_self.GetMovement()->GetPossibleMoves(state[0], m_gridSize, state, possibleLocations, location);

	int loc = -1;
	for (auto v : possibleLocations)
	{
		loc = v.first;
		random -= v.second;
		if (random <= 0.0)
			break;
	}

	state[0] = loc;
}

int nxnGridGlobalActions::MoveFromLocation(intVec & state, Coordinate & goFrom) const
{
	Coordinate self(state[0] % m_gridSize, state[0] / m_gridSize);

	int maxLocation = state[0];
	int maxDist = self.Distance(goFrom);

	// run on all possible move locations and find the farthest point from goFrom
	for (size_t i = 0; i < s_numMoves; ++i)
	{
		Coordinate move(state[0] % m_gridSize, state[0] / m_gridSize);
		move.X() += s_lutDirections[i][0];
		move.Y() += s_lutDirections[i][1];

		// if move is not on grid continue for next move
		if (move.X() < 0 | move.X() >= m_gridSize | move.Y() < 0 | move.Y() >= m_gridSize)
			continue;

		// if move is valid calculate distance
		int currLocation = move.X() + move.Y() * m_gridSize;
		if (ValidLocation(state, currLocation))
		{
			int currDist = goFrom.Distance(move);
			if (currDist > maxDist)
			{
				maxLocation = currLocation;
				maxDist = currDist;
			}
		}
	}

	return maxLocation;
}

int nxnGridGlobalActions::NearestShelter(int loc) const
{
	Coordinate location(loc % m_gridSize, loc / m_gridSize);
	int minDist = INT_MAX;
	int nearestShelter = -1;

	for (int s = 0; s < m_shelters.size(); ++s)
	{
		auto sLoc = m_shelters[s].GetLocation();
		int currDist = location.Distance(sLoc);
		if (currDist < minDist)
		{
			currDist = minDist;
			nearestShelter = sLoc.GetIdx(m_gridSize);
		}
	}

	return nearestShelter;
}

bool nxnGridGlobalActions::EnemyRelatedAction(int action) const
{
	return action >= NumBasicActions();
}

} //end ns despot