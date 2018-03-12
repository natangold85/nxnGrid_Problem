#include <string>
#include <math.h>



#include "nxnGrid.h"
#include "Coordinate.h"

namespace despot 
{
// prams for tree sending
enum TREE_SEND_PARAMS { END_TREE, START_NEW_TRIAL, START_NEW_STEP, END_RUN };

// init static members

// state params
int DetailedState::s_numNonInvolved = 0;
int DetailedState::s_numEnemies = 0;

int DetailedState::s_gridSize = 0;
int DetailedState::s_targetLoc = 0;

std::vector<int> DetailedState::s_shelters;


// model params
std::vector<std::string> nxnGrid::s_actionsStr;
int nxnGrid::s_lutGridSize = -1;
int nxnGrid::s_periodOfDecision = 1;


bool nxnGrid::s_isOnline = true;
bool nxnGrid::s_parallelRun = true;
bool nxnGrid::s_resampleFromLastObs = true;
bool nxnGrid::s_isExternalSimulator = false;
bool nxnGrid::s_toSendTree = false;

std::vector<nxnGrid::intVec> nxnGrid::s_objectsInitLocations;

UDP_Server nxnGrid::s_udpVBS;
UDP_Server nxnGrid::s_udpTree;

// rewards
const double nxnGrid::REWARD_WIN = 50.0;
const double nxnGrid::REWARD_LOSS = -100.0;
const double nxnGrid::REWARD_KILL_ENEMY = 0.0;
const double nxnGrid::REWARD_KILL_NINV = REWARD_LOSS;
const double nxnGrid::REWARD_ILLEGAL_MOVE = 0;
const double nxnGrid::REWARD_MIN = min(REWARD_LOSS, REWARD_ILLEGAL_MOVE);
const double nxnGrid::REWARD_STEP = 0;
const double nxnGrid::REWARD_FIRE = 0;

OBS_TYPE nxnGrid::s_nonObservedState = 0;

// for lut
nxnGrid::lut_t nxnGrid::s_LUT;
enum nxnGrid::CALCULATION_TYPE nxnGrid::s_calculationType = nxnGrid::WITHOUT;

// for memory allocating
std::mutex nxnGrid::s_memoryMutex;
MemoryPool<nxnGridState> nxnGrid::memory_pool_;

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

template<class T>
void FillInBuffer(char ** buffer, T val)
{
	T * ptr = reinterpret_cast<T *>(*buffer);
	*ptr = val;
	*buffer += sizeof(T);
}


/* =============================================================================
* nxnGridState Functions
* =============================================================================*/

nxnGridState::nxnGridState(STATE_TYPE state_id, double weight)
	: State(state_id, weight)
{
}

std::string nxnGridState::text() const
{
	DetailedState state(state_id);
	return state.text();
}

DetailedState::DetailedState()
: m_locations()
{
}

DetailedState::DetailedState(unsigned int sizeState)
: m_locations(sizeState)
{
}

DetailedState::DetailedState(const State & state)
: m_locations()
{
	InitLocationsFromId(state.state_id, m_locations);
}

DetailedState::DetailedState(STATE_TYPE stateId)
: m_locations()
{
	InitLocationsFromId(stateId, m_locations);
}

void DetailedState::InitLocationsFromId(STATE_TYPE state_id, intVec & locations)
{
	int numObjects = 1 + s_numEnemies + s_numNonInvolved;
	locations.resize(numObjects);

	// retrieve each location according to s_NUM_BITS_LOCATION
	int bitsForSinglelocation = (1 << s_NUM_BITS_LOCATION) - 1;
	for (int obj = 0; obj < numObjects; ++obj)
	{
		locations[obj] = state_id & bitsForSinglelocation;
		state_id >>= s_NUM_BITS_LOCATION;
	}
}

int DetailedState::GetObjLocation(STATE_TYPE state_id, int objIdx)
{
	int locationBits = (1 << s_NUM_BITS_LOCATION) - 1;
	state_id >>= objIdx * s_NUM_BITS_LOCATION;

	return state_id & locationBits;
}

int DetailedState::GetObservedObjLocation(OBS_TYPE obs_id, int objIdx)
{
	int locationBits = (1 << s_NUM_BITS_LOCATION) - 1;
	obs_id >>= objIdx * s_NUM_BITS_LOCATION;

	return obs_id & locationBits;
}


STATE_TYPE DetailedState::GetStateId() const
{
	STATE_TYPE stateId = 0;
	for (int obj = m_locations.size() - 1; obj >= 0; --obj)
	{
		stateId <<= s_NUM_BITS_LOCATION;
		stateId |= m_locations[obj];
	}

	return stateId;
}

OBS_TYPE DetailedState::GetObsId() const
{
	return static_cast<OBS_TYPE>(GetStateId());
}

double DetailedState::ObsProb(STATE_TYPE state_id, OBS_TYPE obs, int gridSize, const Observation & obsType)
{
	DetailedState state(state_id);
	DetailedState obsState(obs);

	// if observation is not including the location of the robot return 0
	if (state.m_locations[0] != obsState.m_locations[0])
		return 0.0;

	double pObs = 1.0;
	// run on all non-self objects location
	for (int i = 1; i < s_numEnemies + s_numNonInvolved; ++i)
	{
		// create possible observation of obj location
		Observation::observableLocations observableLocations;
		obsType.InitObsAvailableLocations(state.m_locations[0], state.m_locations[i], gridSize, observableLocations);

		// run on possible observable location
		bool isObserved = false;
		for (auto obsLoc : observableLocations)
		{
			if (obsLoc.first == obsState.m_locations[i])
			{
				pObs *= obsLoc.second;
				isObserved = true;
			}
		}

		// if the object is not in observable locations return 0
		if (!isObserved)
			return 0.0;
	}

	return pObs;
}

double DetailedState::ObsProbOneObj(STATE_TYPE state_id, OBS_TYPE obs, int objIdx, int gridSize, const Observation & obsType)
{
	int selfLoc = GetObjLocation(state_id, 0);
	
	if (objIdx == 0)
		return selfLoc == GetObservedObjLocation(obs, 0);

	int objLoc = GetObjLocation(state_id, objIdx);
	int observedLoc = GetObservedObjLocation(obs, objIdx);
	double pObs = 0.0;

	// create possible observation of obj location
	Observation::observableLocations observableLocations;
	// send location of self of observed state for non observable location(only important location of object not self)
	obsType.InitObsAvailableLocations(selfLoc, objLoc, gridSize, observableLocations);

	// run on possible observable location
	for (auto obsLoc : observableLocations)
	{
		if (obsLoc.first == observedLoc)
			return obsLoc.second;
	}

	return 0.0;
}

void DetailedState::InitStatic()
{
	s_numNonInvolved = 0;
	s_numEnemies = 0;
	s_gridSize = 0;
	s_shelters.resize(0);
}

STATE_TYPE DetailedState::MaxState()
{
	int numObjects = 1 + s_numEnemies + s_numNonInvolved;
	int numBits = s_NUM_BITS_LOCATION * numObjects;

	STATE_TYPE ret = 1;
	return ret << numBits;
}

char DetailedState::ObjIdentity(const intVec & locations, int location)
{
	if (locations[0] == location)
		return 'M';

	int o = 1;

	for (; o < s_numEnemies + 1; ++o)
	{
		if (locations[o] == location)
			return o + '0';
	}

	int toEndOfNonInv = o + s_numNonInvolved;
	for (o; o < toEndOfNonInv; ++o)
	{
		if (locations[o] == location)
			return 'N';
	}

	for (int s = 0; s < s_shelters.size(); ++s)
	{
		if (location == s_shelters[s])
			return 'S';
	}

	if (location == s_targetLoc)
		return 'T';

	return '_';
}

void DetailedState::EraseNonInv()
{
	int startNonInv = 1 + s_numEnemies;
	for (int n = 0; n < s_numNonInvolved; ++n)
		EraseObject(startNonInv);
}

void DetailedState::EraseObject(int objectIdx)
{
	m_locations.erase(m_locations.begin() + objectIdx);
}

bool DetailedState::IsProtected(int objIdx) const
{
	bool ret = false;
	for (auto shelter : s_shelters)
		ret |= shelter == m_locations[objIdx];

	return ret;
}

std::string DetailedState::text() const
{
	std::string ret = "(";
	for (int i = 0; i < m_locations.size(); ++i)
	{
		ret += std::to_string(m_locations[i]) + ", ";
	}
	ret += ")\n";

	PrintGrid(m_locations, ret);
	return ret;
}

void DetailedState::PrintGrid(const intVec & locations, std::string & buffer)
{
	for (int y = 0; y < s_gridSize; ++y)
	{
		for (int x = 0; x < s_gridSize; ++x)
		{
			int loc = x + y * s_gridSize;
			buffer += ObjIdentity(locations, loc);
		}
		buffer += "\n";
	}
}

bool DetailedState::NoEnemies(int gridSize) const
{
	for (int e = 0; e < s_numEnemies; ++e)
		if (!Attack::IsDead(m_locations[e + 1], gridSize))
			return false;

	return true;
}

bool DetailedState::IsNonInvDead(int gridSize) const
{
	bool anyDead = false;
	int offsetToNonInv = 1 + s_numEnemies;
	for (int n = 0; n < s_numNonInvolved && !anyDead; ++n)
		anyDead = Attack::IsDead(m_locations[offsetToNonInv + n], gridSize);

	return anyDead;
}

//bool DetailedState::NonValidState() const
//{
//	for (auto loc : m_locations)
//	{
//		if (loc > Observation::NonObservedLoc(s_gridSize) || loc < 0)
//			return true;
//	}
//	return false;
//}
/* =============================================================================
* nxnGrid Functions
* =============================================================================*/

nxnGrid::nxnGrid(int gridSize, int target, Self_Obj & self, std::vector<intVec> & objectsInitLoc)
	: m_gridSize(gridSize)
	, m_targetIdx(target)
	, m_self(self)
	, m_enemyVec()
	, m_shelters()
	, m_nonInvolvedVec()
{
	// init size  of state for nxnGridstate
	DetailedState::s_gridSize = gridSize;
	DetailedState::s_targetLoc = target;

	s_objectsInitLocations = objectsInitLoc;
}

bool nxnGrid::InitUDP(int vbsPort, int sendTreePort)
{
	bool stat = true;
	if (vbsPort > 0)
	{
		stat &= s_udpVBS.Init(vbsPort);
		s_isExternalSimulator = true;
	}
	else
		s_isExternalSimulator = false;

	if (sendTreePort > 0)
	{
		stat &= s_udpTree.Init(sendTreePort);
		char c;
		s_udpTree.Read(&c);
		s_toSendTree = c;
	}
	else
		s_toSendTree = false;

	if (stat)
		std::cout << "required udp connections initialized\n";
	else
		std::cout << "udp connections not initialized correctly\n";

	return stat;
}

void nxnGrid::InitStaticMembers(bool isOnline, bool parallelRun, int periodOfDecision, bool resampleFromLastObs)
{
	s_isOnline = isOnline;
	s_parallelRun = parallelRun;
	s_periodOfDecision = periodOfDecision;
	s_resampleFromLastObs = resampleFromLastObs;
}

void nxnGrid::InitLUT(lut_t & offlineLut, int offlineGridSize, CALCULATION_TYPE ctype)
{
	s_calculationType = ctype;
	s_lutGridSize = offlineGridSize;
	s_LUT = offlineLut;
}

std::vector<State*> nxnGrid::ResampleLastObs(int num, const std::vector<State*>& belief, const DSPOMDP* model, History history)
{
	// randomization regarding choosing particles
	double unit = 1.0 / num;
	double mass = Random::RANDOM.NextDouble(0, unit);
	int pos = 0;
	double cur = belief[0]->weight;

	// for step
	double reward;
	OBS_TYPE obs;

	auto modelCast = static_cast<const nxnGrid *>(model);
	std::vector<std::vector<std::pair<int, double>>> objLocations(modelCast->CountMovingObjects());


	int nonObservedLoc = Observation::NonObservedLoc(modelCast->GetGridSize());
	// to get num particles need to take from each observed object nth root of num particles
	double numObjLocations = pow(num, 1.0 / (modelCast->CountMovingObjects() - 1));
	// insert self location (is known)
	objLocations[0].emplace_back(DetailedState::GetObservedObjLocation(history.LastObservation(), 0), 1.0);
	// run on each observable object and create individualy for each object belief state
	for (int obj = 1; obj < modelCast->CountMovingObjects(); ++obj)
	{
		int startSim = history.Size();
		
		int loc = nonObservedLoc;

		// running on all history and search for the last time observed the object
		for (; startSim > 0 & loc == nonObservedLoc; --startSim)
			loc = DetailedState::GetObservedObjLocation(history.Observation(startSim - 1), obj);

		State* observedLoc = nullptr;

		// if we observe the object remember state as initial condition
		if (loc != nonObservedLoc)
		{
			observedLoc = model->Allocate(history.Observation(startSim), 1.0);
			++startSim;  // start simulate step after the observation
		}

		int trial = 0;
		int count = 0;

		while (count < numObjLocations && trial < 200 * numObjLocations)
		{
			State* particle;
			OBS_TYPE prevObs;
			// if we didn't observed the object start from initial belief
			if (observedLoc == nullptr)
			{
				// Pick next particle
				while (mass > cur)
				{
					pos = (pos + 1) % belief.size();
					cur += belief[pos]->weight;
				}
				trial++;

				mass += unit;

				particle = model->Copy(belief[pos]);
				prevObs = NonObservedState();
			}
			else
			{
				particle = model->Copy(observedLoc);
				prevObs = history.Observation(startSim - 1);
			}

			// Step through history
			double wgt = 1.0;

			for (int i = startSim; i < history.Size(); i++)
			{
				model->Step(*particle, Random::RANDOM.NextDouble(), history.Action(i), prevObs, reward, obs);
				double prob = modelCast->ObsProbOneObj(history.Observation(i), *particle, history.Action(i), obj);
				if (prob <= 0)
				{
					model->Free(particle);
					break;
				}
				wgt *= prob;
				prevObs = history.Observation(i);
			}

			// Add to obj available locations if survived
			if (particle->IsAllocated())
			{
				count++;
				int objLoc = DetailedState::GetObjLocation(particle->state_id, obj);
				objLocations[obj].emplace_back(objLoc, wgt);
			}
		}

		if (observedLoc != nullptr)
			model->Free(observedLoc);
	}

	std::vector<State*> sample;
	// create sample from object possible locations
	modelCast->CreateParticleVec(objLocations, sample);

	double total_weight = 0;
	for (int i = 0; i < sample.size(); i++) {
		//sample[i]->weight = exp(sample[i]->weight - max_wgt);
		total_weight += sample[i]->weight;
	}
	for (int i = 0; i < sample.size(); i++) {
		sample[i]->weight = sample[i]->weight / total_weight;
	}

	return sample;
}

void nxnGrid::UpdateRealAction(int & action)
{
	static int counter = 0;
	static int prevAction = 0;

	if (counter % s_periodOfDecision == 0)
		prevAction = action;
	else
		action = prevAction;

	++counter;
}

void nxnGrid::SendEndRun()
{
	unsigned int buf[1];

	buf[0] = END_RUN;
	s_udpTree.Write(reinterpret_cast<char *>(buf), 1 * sizeof (int));
}


void nxnGrid::SendModelDetails(int numActions, int numObj)
{
	// insert to unsigned int for sending tree compatibility
	unsigned int buf[4];

	buf[0] = START_NEW_TRIAL;
	buf[1] = numActions;
	buf[2] = numObj;
	buf[3] = END_TREE;

	s_udpTree.Write(reinterpret_cast<char *>(buf), 4 * sizeof (int));
}

void nxnGrid::SendTree(State * state, VNode *root)
{
	// create tree
	std::vector<unsigned int> buffer;

	buffer.emplace_back(START_NEW_STEP);

	// TODO : insert state to buffer

	SendTreeRec(root, 0, 1, buffer);

	buffer.emplace_back(END_TREE);

	// send to client
	int sent = 0;
	// to make sure bufsize is divided by sizeof int
	int maxWrite = UDP_Server::s_BUF_LEN - UDP_Server::s_BUF_LEN % sizeof(int);
	while ( sent < (buffer.size()) )
	{
		char *ptr = reinterpret_cast<char *>(&buffer[sent]);
		int leftToWrite = buffer.size() - sent;
		int size = Min(leftToWrite * sizeof(int), maxWrite);
		s_udpTree.Write(ptr, size);
		sent += size / sizeof(int);
	}
}

unsigned int nxnGrid::SendTreeRec(VNode *node, unsigned int parentId, unsigned int id, std::vector<unsigned int> & buffer)
{
	if (node == nullptr || node->count() == 0)
		return id - 1;

	float val = node->value();
	unsigned int count = node->count();

	buffer.emplace_back(parentId);
	buffer.emplace_back(id);
	buffer.emplace_back(*reinterpret_cast<int *>(&val));
	buffer.emplace_back(count);

	int numch = node->children().size() + 1;
	int nextId = id + node->children().size();
	int actionId = id + 1;
	for (auto action : node->children())
	{
		nextId = SendTreeRec(action, id, actionId, nextId, buffer);
		++actionId;
	}

	return nextId;
}

unsigned int nxnGrid::SendTreeRec(QNode *node, unsigned int parentId, unsigned int id, unsigned int nextAvailableId, std::vector<unsigned int> & buffer)
{
	if (node == nullptr) 
		return id; // for action node need to remember id for completion of non initialized nodes

	float val = node->value();
	unsigned int count = node->count();

	buffer.emplace_back(parentId);
	buffer.emplace_back(id);
	buffer.emplace_back(*reinterpret_cast<int *>(&val));
	buffer.emplace_back(count);

	int nextId = nextAvailableId;
	for (auto obs : node->children())
	{
		++nextId;
		nextId = SendTreeRec(obs.second, id, nextId, buffer);
	}

	return nextId;
}

void nxnGrid::AddObj(Attack_Obj&& obj)
{
	m_enemyVec.emplace_back(std::forward<Attack_Obj>(obj));
	
	DetailedState::s_numEnemies = m_enemyVec.size();
	s_nonObservedState = GetNonObservedState();

	AddActionsToEnemy();
}

void nxnGrid::AddObj(Movable_Obj&& obj)
{
	m_nonInvolvedVec.emplace_back(std::forward<Movable_Obj>(obj));

	DetailedState::s_numNonInvolved = m_nonInvolvedVec.size();
	s_nonObservedState = GetNonObservedState();

}

void nxnGrid::AddObj(ObjInGrid&& obj)
{
	m_shelters.emplace_back(std::forward<ObjInGrid>(obj));
	
	DetailedState::s_shelters.emplace_back(obj.GetLocation().GetIdx(m_gridSize));
	s_nonObservedState = GetNonObservedState();

	AddActionsToShelter();
}


double nxnGrid::ObsProb(OBS_TYPE obs, const State & s, int action) const
{
	return DetailedState::ObsProb(s.state_id, obs, m_gridSize, *m_self.GetObservation());
}

double nxnGrid::ObsProbOneObj(OBS_TYPE obs, const State & s, int action, int objIdx) const
{
	return DetailedState::ObsProbOneObj(s.state_id, obs, objIdx, m_gridSize, *m_self.GetObservation());
}

OBS_TYPE nxnGrid::GetNonObservedState() const
{
	unsigned int numObjInState = CountMovingObjects();
	DetailedState nonObservedState(numObjInState);
	
	for (int o = 0; o < numObjInState; ++o)
		nonObservedState[o] = Observation::NonObservedLoc(m_gridSize);

	return nonObservedState.GetObsId();
}

void nxnGrid::CreateParticleVec(std::vector<std::vector<std::pair<int, double> > > & objLocations, std::vector<State*> & particles) const
{
	DetailedState state(CountMovingObjects());
	CreateParticleVecRec(state, objLocations, particles, 1.0, 0);
}

void nxnGrid::ChoosePreferredActionIMP(const DetailedState & beliefState, doubleVec & expectedReward) const
{
	lut_t::iterator itr, itr2;


	switch (s_calculationType)
	{
	case ALL:
	{
		DetailedState scaledState(beliefState.size());

		ScaleState(beliefState, scaledState);
		itr = s_LUT.find(scaledState.GetStateId());
		if (itr != s_LUT.end())
			expectedReward = itr->second;
		else
			expectedReward = doubleVec(NumActions(), REWARD_LOSS);
		break;
	}
	case WO_NINV:
	{
		// erase all non involved
		DetailedState woNInv(beliefState);
		woNInv.EraseNonInv();

		DetailedState scaledState(woNInv.size());
		ScaleState(woNInv, scaledState);
		itr = s_LUT.find(scaledState.GetStateId());
		if (itr != s_LUT.end())
			expectedReward = itr->second;
		else
			expectedReward = doubleVec(NumActions(), REWARD_LOSS);
		break;
	}
	case JUST_ENEMY:
	{
		// erase all non involved
		DetailedState woNInv(beliefState);
		woNInv.EraseNonInv();
		DetailedState scaledState(woNInv.size());

		ScaleState(woNInv, scaledState);
		itr = s_LUT.find(scaledState.GetStateId());
		if (itr != s_LUT.end())
			expectedReward = itr->second;
		else
			expectedReward = doubleVec(NumActions(), REWARD_LOSS);
		break;
	}

	case ONE_ENEMY:
	{
		// erase all non involved
		DetailedState firstEnemy(beliefState);
		firstEnemy.EraseNonInv();
		DetailedState secondEnemy(firstEnemy);

		firstEnemy.EraseObject(1);
		secondEnemy.EraseObject(2);
		DetailedState scaledState(firstEnemy.size());

		ScaleState(firstEnemy, scaledState);
		itr = s_LUT.find(scaledState.GetStateId());

		// calculate reward with second enemy
		ScaleState(secondEnemy, scaledState);
		itr2 = s_LUT.find(scaledState.GetStateId());

		if (itr != s_LUT.end() & itr2 != s_LUT.end())
			Combine2EnemiesRewards(beliefState.m_locations, itr->second, itr2->second, expectedReward);
		else
			expectedReward = doubleVec(NumActions(), REWARD_LOSS);
		break;
	}
	
	case WO_NINV_STUPID:
	{
		// TODO : change implementation of stupid alg
		//beliefState.erase(beliefState.begin() + 1 + m_enemyVec.size());
		//scaledState.resize(beliefState.size());
		//ScaleState(beliefState, scaledState);
		//itr = s_LUT.find(nxnGridState::StateToIdx(scaledState, s_lutGridSize));
		//
		//// TODO : move members 1 slot right
		//if (itr != s_LUT.end())
		//	expectedReward = itr->second;
		//else
		//	expectedReward = doubleVec(NumActions(), REWARD_LOSS);
		break;
	}
	default: // calc type = WITHOUT
		expectedReward = doubleVec(NumActions(), REWARD_LOSS);
		if (REWARD_ILLEGAL_MOVE < REWARD_LOSS)
		{
			for (int a = 0; a < NumActions(); ++a)
			{
				if (!LegalAction(beliefState, a))
					expectedReward[a] = REWARD_ILLEGAL_MOVE;
			}
		}
		break;
	}
}

int nxnGrid::FindMaxReward(const doubleVec & rewards, double & expectedReward)
{
	int maxAction = -1;
	double maxReward = REWARD_LOSS - 1;

	for (int a = 0; a < rewards.size(); ++a)
	{
		if (rewards[a] > maxReward)
		{
			maxAction = a;
			maxReward = rewards[a];
		}
	}

	expectedReward = maxReward;
	return maxAction;
}

void nxnGrid::CreateParticleVecRec(DetailedState & state, std::vector<std::vector<std::pair<int, double> > > & objLocations, std::vector<State*> & particles, double weight, int currIdx) const
{
	if (currIdx == state.size())
	{
		auto beliefState = static_cast<nxnGridState*>(Allocate(state.GetStateId(), weight));
		particles.push_back(beliefState);
	}
	else
	{
		for (auto loc : objLocations[currIdx])
		{
			state[currIdx] = loc.first;
			CreateParticleVecRec(state, objLocations, particles, weight * loc.second, currIdx + 1);
		}
	}
}

State * nxnGrid::CreateStartState(std::string type) const
{
	DetailedState state(CountMovingObjects());

	state[0] = m_self.GetLocation().GetIdx(m_gridSize);
	
	for (int i = 0; i < m_enemyVec.size(); ++i)
		state[i + 1] = m_enemyVec[i].GetLocation().GetIdx(m_gridSize);

	for (int i = 0; i < m_nonInvolvedVec.size(); ++i)
		state[i + 1 + m_enemyVec.size()] = m_nonInvolvedVec[i].GetLocation().GetIdx(m_gridSize);


	return new nxnGridState(state.GetStateId());
}

Belief * nxnGrid::InitialBelief(const State * start, std::string type) const
{
	std::vector<State*> particles;
	DetailedState state(CountMovingObjects());

	// create belief states given self location
	state[0] = m_self.GetLocation().GetIdx(m_gridSize);
	int numStates = 1;
	// assumption : init states cannot be in same locations for different object
	for (int i = 1; i < CountMovingObjects(); ++i)
		numStates *= s_objectsInitLocations[i].size();

	double stateProb = 1.0 / numStates;
	
	InitialBeliefStateRec(state, 1, stateProb, particles);

	return new ParticleBelief(particles, this);
}

State * nxnGrid::Allocate(STATE_TYPE state_id, double weight) const
{
	nxnGridState* particle = nullptr;
	{
		std::lock_guard<std::mutex> lock(s_memoryMutex);
		particle = memory_pool_.Allocate();
	}

	particle->state_id = state_id;
	particle->weight = weight;
	return particle;
}

State * nxnGrid::Copy(const State * particle) const
{
	nxnGridState* new_particle = nullptr;
	{
		std::lock_guard<std::mutex> lock(s_memoryMutex);
		new_particle = memory_pool_.Allocate();
	}

	*new_particle = *static_cast<const nxnGridState*>(particle);
	new_particle->SetAllocated();
	return new_particle;
}

void nxnGrid::Free(State * particle) const
{
	std::lock_guard<std::mutex> lock(s_memoryMutex);
	memory_pool_.Free(static_cast<nxnGridState*>(particle));
}

int nxnGrid::NumActiveParticles() const
{
	return memory_pool_.num_allocated();
}

void nxnGrid::DisplayParameters(std::ofstream & out) const
{
	out << "\n\nModel Details:\n";
	out << "grid size: " + std::to_string(m_gridSize) + "  target idx: " + std::to_string(m_targetIdx);
	out << "\n\nSELF:\nself possible initial locations: (";
	for (auto loc : s_objectsInitLocations[0])
		out << loc << ", ";
	out << ")";
	out << "\nwith move properties: " + m_self.GetMovement()->String();
	out << "\nwith attack: " + m_self.GetAttack()->String();
	out << "\nwith observation: " + m_self.GetObservation()->String();
	
	out << "\n\nENEMIES:\n";
	for (int i = 0; i < m_enemyVec.size(); ++i)
	{
		out << "\nenemy #" << i;
		out << "\nobject possible initial locations : (";
		for (auto loc : s_objectsInitLocations[i + 1])
			out << loc << ", ";
		out << ")";
		out << "\nwith move properties: " + m_enemyVec[i].GetMovement()->String();
		out << "\nwith attack: " + m_enemyVec[i].GetAttack()->String();
	}

	out << "\n\nNON-INVOLVED:\n";
	for (int i = 0; i < m_nonInvolvedVec.size(); ++i)
	{
		out << "\nnon-involved #" << i;
		out << "\nobject possible initial locations : (";
		for (auto loc : s_objectsInitLocations[i + 1 + m_enemyVec.size()])
			out << loc << ", ";
		out << ")";
		out << "\nwith move properties: " + m_nonInvolvedVec[i].GetMovement()->String();
	}

	out << "\n\nSHELTERS:\n";
	for (int i = 0; i < m_shelters.size(); ++i)
	{
		out << "\nshelter #" << i;
		out << "\nobject possible initial locations : (";
		for (auto loc : s_objectsInitLocations[i + 1 + m_enemyVec.size() + m_nonInvolvedVec.size()])
			out << loc << ", ";
		out << ")";
	}
}

void nxnGrid::PrintState(const State & s, std::ostream & out) const
{
	nxnGridState state(s.state_id);
	out << state.text() << "\n";
}

void nxnGrid::PrintBelief(const Belief & belief, std::ostream & out) const
{
}

void nxnGrid::PrintObs(const State & state, OBS_TYPE obs, std::ostream & out) const
{
	nxnGridState obsState(obs);
	out << obsState.text() << "\n";
}

void nxnGrid::PrintAction(int action, std::ostream & out) const
{
	out << s_actionsStr[action] << std::endl;
}

bool nxnGrid::InRange(int locationSelf, int locationObj, double range, int gridSize)
{
	Coordinate self(locationSelf % gridSize, locationSelf / gridSize);
	Coordinate enemy(locationObj % gridSize, locationObj / gridSize);

	// return true when distance is <= from range
	return self.RealDistance(enemy) <= range;
}

bool nxnGrid::CalcIfKilledByEnemy(const DetailedState & state, int enemyIdx, double & randomNum) const
{
	bool isSelfDead = false;
	int enemyStateIdx = enemyIdx + 1;
	if (m_enemyVec[enemyIdx].GetAttack()->InRange(state[enemyStateIdx], state[0], m_gridSize))
	{
		intVec shelters(GetSheltersVec());
		intVec objLocations(state.LocationVec());

		m_enemyVec[enemyIdx].GetAttack()->AttackOnline(objLocations, enemyStateIdx, objLocations[0], shelters, m_gridSize, randomNum);
		// assumption : other objects beside self cannot be killed by enemies
		isSelfDead = Attack::IsDead(objLocations[0], m_gridSize);
	}
	return isSelfDead;
}

void nxnGrid::MoveNonProtectedShelters(const intVec & beliefState, intVec & scaledState, int oldGridSize, int newGridSize) const
{
	int startShelter = 1 + NumEnemiesInCalc() + NumNonInvInCalc();
	for (int s = 0; s < m_shelters.size(); ++s)
	{
		int shelterIdx = s + startShelter;
		
		for (int o = 0; o < startShelter; ++o)
		{
			if ((scaledState[o] == scaledState[shelterIdx]) & (beliefState[o] != beliefState[shelterIdx] & !Attack::IsDead(scaledState[o], newGridSize)))
			{
				MoveObjectLocation(beliefState, scaledState, o, oldGridSize, newGridSize);
			}
		}
	}
}

void nxnGrid::MoveObjectLocation(const intVec & beliefState, intVec & scaledState, int objIdx, int oldGridSize, int newGridSize) const
{
	// TODO : change ScaleState after allowing repetitions and using detailedstate

	//Coordinate objectRealLoc(beliefState[objIdx] % oldGridSize, beliefState[objIdx] / oldGridSize);
	//Coordinate objectLoc(scaledState[objIdx] % newGridSize, scaledState[objIdx] / newGridSize);
	//
	//int minDist = 100000;
	//int bestLocation = -1;
	//double scaleBackward = static_cast<double>(newGridSize) / oldGridSize;

	//// run on all possible moves of enemy and calculate which valid location is closest to object
	//for (size_t i = 0; i < s_numMoves; ++i)
	//{
	//	Coordinate newLoc(s_lutDirections[i][0], s_lutDirections[i][1]);
	//	newLoc += objectLoc;
	//	if (ValidLegalLocation(scaledState, newLoc, objIdx, newGridSize))
	//	{
	//		newLoc /= scaleBackward;
	//		int dist = newLoc.Distance(objectRealLoc);
	//		if (dist < minDist)
	//		{
	//			minDist = dist;
	//			bestLocation = i;
	//		}
	//	}
	//}
	//
	//// TODO: see whats happenning to shelter after the moving of the object
}

void nxnGrid::ShiftSelfFromTarget(const intVec & beliefState, intVec & scaledState, int oldGridSize, int newGridSize) const
{
	Coordinate realSelf(beliefState[0] % oldGridSize, beliefState[0] / oldGridSize);
	
	// if x > y shift on y axis else shift object on x axis
	if (realSelf.X() > realSelf.Y())
	{
		for (int i = 0; i < scaledState.size(); ++i)
		{
			if (Attack::IsDead(scaledState[i], newGridSize))
				continue;

			int yLoc = scaledState[i] / newGridSize - 1;

			// if the object is out of grid drop him from calculation
			if (yLoc < 0)
			{
				scaledState[i] = Attack::DeadLoc(newGridSize);
			}
			else
			{
				scaledState[i] -= newGridSize;
			}
		}
	}
	else
	{
		for (int i = 0; i < scaledState.size(); ++i)
		{
			if (Attack::IsDead(scaledState[i], newGridSize))
				continue;

			int xLoc = scaledState[i] % newGridSize - 1;

			// if the object is out of grid drop him from calculation
			if (xLoc < 0)
			{
				scaledState[i] = Attack::DeadLoc(newGridSize);
			}
			else
			{
				--scaledState[i];
			}
		}
	}
}

int nxnGrid::NumEnemiesInCalc() const
{
	// TODO : make this function more modular
	return m_enemyVec.size() * (s_calculationType == WO_NINV) + 1 * (s_calculationType == ONE_ENEMY);
}

int nxnGrid::NumNonInvInCalc() const 
{
	// TODO : make this function more modular
	return 0;
}

enum nxnGrid::OBJECT nxnGrid::WhoAmI(int objIdx) const
{
	return objIdx == 0 ? SELF :
		objIdx < m_enemyVec.size() + 1 ? ENEMY :
		objIdx < m_nonInvolvedVec.size() + m_enemyVec.size() + 1 ? NON_INV :
		objIdx < m_shelters.size() + m_nonInvolvedVec.size() + m_enemyVec.size() + 1 ? SHELTER : TARGET;
}

void nxnGrid::InitialBeliefStateRec(DetailedState & state, int currObj, double stateProb, std::vector<State*> & particles) const
{
	if (currObj == CountMovingObjects())
	{
		auto beliefState = static_cast<nxnGridState*>(Allocate(state.GetStateId(), stateProb));
		particles.push_back(beliefState);
	}
	else
	{
		for (auto obj : s_objectsInitLocations[currObj])
		{
			state[currObj] = obj;
			InitialBeliefStateRec(state, currObj + 1, stateProb, particles);
		}
	}
}
OBS_TYPE nxnGrid::FindObservation(const DetailedState & state, double p) const
{
	DetailedState obsState(CountMovingObjects());
	obsState[0] = state[0];
	// calculate observed state
	DecreasePObsRec(state, obsState, 1, 1.0, p);

	// return the observed state
	return obsState.GetStateId();
}

void nxnGrid::SetNextPosition(DetailedState & state, doubleVec & randomNum) const
{
	// run on all enemies
	for (int i = 0; i < m_enemyVec.size(); ++i)
	{
		CalcMovement(state, &m_enemyVec[i], randomNum[i], i + 1);
	}
	// run on all non-involved non-involved death is the end of game so don't need to check for death
	for (int i = 0; i < m_nonInvolvedVec.size(); ++i)
	{
		CalcMovement(state, &m_nonInvolvedVec[i], randomNum[i + m_enemyVec.size()], i + 1 + m_enemyVec.size());
	}
}

void nxnGrid::CalcMovement(DetailedState & state, const Movable_Obj *object, double rand, int objIdx) const
{
	// if the p that was pulled is in the pStay range do nothing
	std::map<int, double> possibleLocations;
	intVec nonValidLocations;
	GetNonValidLocations(state, objIdx, nonValidLocations);
	object->GetMovement()->GetPossibleMoves(state[objIdx], m_gridSize, nonValidLocations, possibleLocations, state[0]);

	int loc = -1;
	for (auto v : possibleLocations)
	{
		loc = v.first;
		rand -= v.second;
		if (rand <= 0.0)
			break;
	}

	state[objIdx] = loc;
}

void nxnGrid::ScaleState(const DetailedState & beliefState, DetailedState & scaledState) const
{
	ScaleState(beliefState, scaledState, s_lutGridSize, m_gridSize);
}

void nxnGrid::ScaleState(const DetailedState & beliefState, DetailedState & scaledState, int newGridSize, int oldGridSize) const
{
	// TODO : change ScaleState after allowing repetitions and using detailedstate

	//double scale = static_cast<double>(oldGridSize) / newGridSize;

	//for (size_t i = 0; i < beliefState.size(); ++i)
	//{
	//	Coordinate location(beliefState[i] % oldGridSize, beliefState[i] / oldGridSize);
	//	location /= scale;
	//	scaledState[i] = location.X() + location.Y() * newGridSize;
	//}

	//// if target is in self location shift map to the left or upper so self won't be in target location 
	// TODO : hard coded for target location
	//if (scaledState[0] == newGridSize * newGridSize - 1)
	//	ShiftSelfFromTarget(beliefState, scaledState, oldGridSize, newGridSize);

	//
	//int numEnemies = NumEnemiesInCalc();
	//// run on enemies in calculation if the enemy is in the same spot move enemy to the nearest available location
	//for (int i = 0; i < numEnemies; ++i)
	//{
	//	if (!NoRepetitions(scaledState, i + 1, newGridSize))
	//		MoveObjectLocation(beliefState, scaledState, i + 1, oldGridSize, newGridSize);
	//}

	//// run on non-involved in calculation if the non-involved is in non-valid location move him to the nearest location
	//for (int i = 0; i < NumNonInvInCalc(); ++i)
	//{
	//	if (!NoRepetitions(scaledState, numEnemies + 1 + i, newGridSize))
	//		MoveObjectLocation(beliefState, scaledState, m_enemyVec.size() + 1, oldGridSize, newGridSize);
	//}
	//

	//MoveNonProtectedShelters(beliefState, scaledState, oldGridSize, newGridSize);
}

void nxnGrid::Combine2EnemiesRewards(const intVec & beliefState, const doubleVec & rewards1E, const doubleVec & rewards2E, doubleVec & rewards) const
{
	static int bitEnemy1 = 1;
	static int bitEnemy2 = 2;
	int liveEnemies = 0;
	int numLivingEnemies = 0;
	rewards.resize(NumActions());
	// assumption : only 2 enemies
	int numEnemyActions = NumActions() - rewards1E.size();
	// start with actions related to enemy to understand which enemy is dead and calc mean in simple actions (assumption enemy related actions are at the end of actions)
	for (int a = NumActions() - 1; a >= 0; --a)
	{
		if (EnemyRelatedAction(a))
		{
			int enemyIdx = EnemyRelatedActionIdx(a);
			if (Attack::IsDead(beliefState[enemyIdx], m_gridSize))
				rewards[a] = REWARD_MIN;
			else if (enemyIdx == 1)
			{
				rewards[a] = rewards1E[a];
				liveEnemies |= bitEnemy1;
				++numLivingEnemies;
			}
			else
			{
				rewards[a] = rewards2E[a - numEnemyActions];
				liveEnemies |= bitEnemy2;
				++numLivingEnemies;
			}
		}
		else
		{
			// when only 1 enemy is dead insert to rewards calculation only reward of the live enemy
			if (numLivingEnemies > 0)
				rewards[a] = ((rewards1E[a] * (liveEnemies & bitEnemy1) + rewards2E[a] * (liveEnemies & bitEnemy2))) / numLivingEnemies;
			else
				rewards[a] = (rewards1E[a] + rewards2E[a]) / 2;

		}
	}
}

void nxnGrid::DecreasePObsRec(const DetailedState & originalState, DetailedState & currState, int currIdx, double pToDecrease, double &pLeft) const
{
	if (currIdx == CountMovingObjects())
	{
		// decrease observed state probability from the left probability
		pLeft -= pToDecrease;
	}
	else
	{
		int selfLoc = originalState[0];
		int objLoc = originalState[currIdx];
		Observation::observableLocations obsLocations;
		m_self.GetObservation()->InitObsAvailableLocations(selfLoc, objLoc, m_gridSize, obsLocations);
		for (auto obs : obsLocations)
		{
			currState[currIdx] = obs.first;
			DecreasePObsRec(originalState, currState, currIdx + 1, pToDecrease * obs.second, pLeft);
			if (pLeft < 0)
				return;
		}
	}
}

int nxnGrid::CountAllObjects() const
{
	return CountMovingObjects() + m_shelters.size();
}

unsigned int nxnGrid::CountMovingObjects() const
{
	return 1 + m_enemyVec.size() + m_nonInvolvedVec.size();
}

void nxnGrid::CreateRandomVec(doubleVec & randomVec, int size)
{
	// insert to a vector numbers between 0-1
	for (int i = 0; i < size; ++i)
	{
		randomVec.emplace_back(RandomNum());
	}
}

void nxnGrid::SendAction(int action)
{
	s_udpVBS.Write(reinterpret_cast<char *>(&action), sizeof(int));
}

bool nxnGrid::RcvState(State * s, OBS_TYPE lastObs, double & reward, OBS_TYPE & obs)
{
	DetailedState state(*s);
	DetailedState observation(lastObs);
	InitStateIMP(state, observation);

	if (state[0] == m_targetIdx)
	{
		SendAction(NumActions());
		return true;
	}
	
	s->state_id = state.GetStateId();
	obs = observation.GetStateId();

	// calc rewards according to model
	reward = 0.0;
	DetailedState lastObsState(lastObs);

	if (Attack::IsDead(state[0],m_gridSize) || state.IsNonInvDead(m_gridSize))
		reward = REWARD_LOSS;
	else if (state[0] == m_targetIdx)
		reward = REWARD_WIN;

	return false;
}

void nxnGrid::InitState()
{
	if (ToSendTree())
		SendModelDetails(NumActions(), CountAllObjects());

	if (s_isExternalSimulator)
		InitStateSimulator();
	else
		InitStateRandom();
}

/// init state according to s_objectsInitLocations
void nxnGrid::InitStateRandom()
{
	int obj = 0;
	int idx = rand() % s_objectsInitLocations[obj].size();
	int loc = s_objectsInitLocations[obj][idx];
	m_self.SetLocation(Coordinate(loc % m_gridSize, loc / m_gridSize));
	++obj;

	for (int i = 0; i < m_enemyVec.size(); ++i, ++obj)
	{
		idx = rand() % s_objectsInitLocations[obj].size();
		loc = s_objectsInitLocations[obj][idx];
		m_enemyVec[i].SetLocation(Coordinate(loc % m_gridSize, loc / m_gridSize));
	}
	
	for (int i = 0; i < m_nonInvolvedVec.size(); ++i, ++obj)
	{
		idx = rand() % s_objectsInitLocations[obj].size();
		loc = s_objectsInitLocations[obj][idx];
		m_nonInvolvedVec[i].SetLocation(Coordinate(loc % m_gridSize, loc / m_gridSize));
	}

	for (int i = 0; i < m_shelters.size(); ++i, ++obj)
	{
		idx = rand() % s_objectsInitLocations[obj].size();
		loc = s_objectsInitLocations[obj][idx];
		m_shelters[i].SetLocation(Coordinate(loc % m_gridSize, loc / m_gridSize));
		DetailedState::s_shelters[i] = loc;
	}

}
/// init state from simulator
void nxnGrid::InitStateSimulator()
{
	DetailedState state(CountMovingObjects());
	DetailedState observation(CountMovingObjects());
	InitStateIMP(state, observation);

	int currObj = 0;
	Coordinate self(state[currObj] % m_gridSize, state[currObj] / m_gridSize);
	m_self.SetLocation(self);

	for (int i = 0; i < m_enemyVec.size(); ++i)
	{
		++currObj;
		Coordinate enemy;
		if (state[currObj] == m_gridSize * m_gridSize)
			enemy = Coordinate(m_gridSize, m_gridSize);
		else
			enemy = Coordinate(state[currObj] % m_gridSize, state[currObj] / m_gridSize);

		m_enemyVec[i].SetLocation(enemy);
	}

	for (int i = 0; i < m_nonInvolvedVec.size(); ++i)
	{
		++currObj;
		Coordinate ninv;
		if (state[currObj] == m_gridSize * m_gridSize)
			ninv = Coordinate(m_gridSize, m_gridSize);
		else
			ninv = Coordinate(state[currObj] % m_gridSize, state[currObj] / m_gridSize);

		m_nonInvolvedVec[i].SetLocation(ninv);
	}
}

void nxnGrid::InitStateIMP(DetailedState & state, DetailedState & observation)
{
	int buffer[40];
	int length = s_udpVBS.Read(reinterpret_cast<char *>(buffer));

	int vbsGridSize = buffer[0];

	intVec identity;
	intVec readState;
	// read objects
	for (int obj = 2; obj < length / sizeof(int); obj += 2)
	{
		identity.emplace_back(buffer[obj - 1]);
		readState.emplace_back(buffer[obj]);
	}

	// organize objects according to identity
	unsigned int stateSize = readState.size() - 1;
	DetailedState organizedState(stateSize);
	int obj = 0;
	organizedState[obj] = FindObject(readState, identity, SELF, 0);

	++obj;
	std::vector<bool> isObserved(organizedState.size());
	isObserved[0] = true;

	for (int i = 0; i < m_enemyVec.size(); ++i, ++obj)
	{
		bool isObs;
		organizedState[obj] = FindObject(readState, identity, ENEMY_VBS, OBSERVED_ENEMY_VBS, isObs, i);
		isObserved[obj] = isObs;
	}

	for (int i = 0; i < m_nonInvolvedVec.size(); ++i, ++obj)
	{
		bool isObs;
		organizedState[obj] = FindObject(readState, identity, NON_INVOLVED_VBS, OBSERVED_NON_INVOLVED_VBS, isObs, i);
		isObserved[obj] = isObs;
	}

	// read shelters
	for (int i = 0; i < m_shelters.size(); ++i, ++obj)
		organizedState[obj] = FindObject(readState, identity, SHELTER, i);


	// read and scale target
	int vbsTarget = FindObject(readState, identity, TARGET, 0);
	Coordinate targetC(vbsTarget % vbsGridSize, vbsTarget / vbsGridSize);
	targetC *= static_cast<double>(m_gridSize) / vbsGridSize;
	m_targetIdx = targetC.GetIdx(m_gridSize);
	DetailedState::s_targetLoc = m_targetIdx;

	DetailedState scaledState(organizedState.size());
	ScaleState(organizedState, scaledState, m_gridSize, vbsGridSize);

	// insert to observation and state moving objects
	obj = 0;
	for (; obj < CountMovingObjects(); ++obj)
	{
		state[obj] = scaledState[obj];
		observation[obj] = state[obj] * isObserved[obj] + state[0] * (!isObserved[obj]);
	}

	//insert scaled shelters location
	for (int i = 0; i < m_shelters.size(); ++i, ++obj)
	{
		DetailedState::s_shelters[i] = scaledState[obj];
		m_shelters[i].SetLocation(Coordinate(scaledState[obj] % m_gridSize, scaledState[obj] / m_gridSize));
	}
}

int nxnGrid::FindObject(intVec & state, intVec & identity, int object, int idx)
{
	for (int i = 0; i < state.size(); ++i)
	{
		if (identity[i] == object)
		{
			if (idx == 0)
				return state[i];

			--idx;
		}
	}

	return m_gridSize * m_gridSize;
}

int nxnGrid::FindObject(intVec & state, intVec & identity, int object, int observedObj, bool & isObserved, int idx)
{
	for (int i = 0; i < state.size(); ++i)
	{
		if (identity[i] == object | identity[i] == observedObj)
		{
			if (idx == 0)
			{
				isObserved = identity[i] == observedObj;
				return state[i];
			}
			--idx;
		}
	}

	return m_gridSize * m_gridSize;
}

void nxnGrid::ChoosePreferredAction(POMCPPrior * prior, const DSPOMDP* m, doubleVec & expectedRewards)
{
	const nxnGrid * model = static_cast<const nxnGrid *>(m);
	if (prior->history().Size() > 0)
	{
		DetailedState beliefState(model->CountMovingObjects());
		model->InitBeliefState(beliefState, prior->history());
		model->ChoosePreferredActionIMP(beliefState, expectedRewards);
	}
	else
	{
		expectedRewards = doubleVec(model->NumActions(), REWARD_LOSS);

	}
}

int nxnGrid::ChoosePreferredAction(POMCPPrior * prior, const DSPOMDP* m, double & expectedReward)
{	
	if (s_calculationType != WITHOUT)
	{
		doubleVec rewards;
		ChoosePreferredAction(prior, m, rewards);
		// if calc type != without return lut result else return random decision
		return FindMaxReward(rewards, expectedReward);
	}
	else
	{
		// unknown reward for random action
		expectedReward = REWARD_LOSS;
		const nxnGrid * model = static_cast<const nxnGrid *>(m);
		
		DetailedState observedState(prior->history().LastObservation());
		
		// return rand action (if the action is not legal return other random action)
		int randAction;
		do
		{
			randAction = rand() % model->NumActions();
		} while (!model->LegalAction(observedState, randAction));

		return randAction;
	}
}

void nxnGrid::InitBeliefState(DetailedState & beliefState, const History & h) const
{
	OBS_TYPE obs = h.LastObservation();
	DetailedState observedState(obs);
	
	beliefState[0] = observedState[0];
	
	int obj = 1;
	for (; obj < CountMovingObjects() ; ++obj)
	{
		// run on all history and initialize state with last location observed on object
		int currObs = h.Size() - 1;
		int loc = observedState[obj];
		while (Observation::IsNonObserved(loc, m_gridSize) && currObs >= 0)
		{
			loc = DetailedState::GetObservedObjLocation(h.Observation(currObs), obj);
			--currObs;
		}
		// if the object is non observed through all history treat it as dead
		beliefState[obj] = Observation::IsNonObserved(loc, m_gridSize) ? Attack::DeadLoc(m_gridSize) : observedState[obj];
	}
}

intVec nxnGrid::GetSheltersVec() const
{
	intVec shelters;
	for (auto v : m_shelters)
		shelters.emplace_back(v.GetLocation().GetIdx(m_gridSize));

	return shelters;
}

void nxnGrid::GetNonValidLocations(const DetailedState & state, int objIdx, intVec & nonValLoc) const
{
	for (int obj = 0; obj < CountMovingObjects(); ++obj)
	{
		if (obj != objIdx && state.IsProtected(obj))
			nonValLoc.emplace_back(state[obj]);
	}
}

} //end ns despot