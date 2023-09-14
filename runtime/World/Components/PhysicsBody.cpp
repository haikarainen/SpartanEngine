/*
Copyright(c) 2016-2023 Panos Karabelas

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
copies of the Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions :

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR
COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/

//= INCLUDES ================================================
#include "pch.h"
#include "PhysicsBody.h"
#include "Transform.h"
#include "Collider.h"
#include "Constraint.h"
#include "../Entity.h"
#include "../../Physics/Physics.h"
#include "../../Physics/BulletPhysicsHelper.h"
#include "../../IO/FileStream.h"
SP_WARNINGS_OFF
#include "BulletDynamics/Dynamics/btRigidBody.h"
#include "BulletCollision/CollisionShapes/btCollisionShape.h"
SP_WARNINGS_ON
//===========================================================

//= NAMESPACES ===============
using namespace std;
using namespace Spartan::Math;
//============================

namespace Spartan
{
    static const float DEFAULT_MASS              = 0.0f;
    static const float DEFAULT_FRICTION          = 0.5f;
    static const float DEFAULT_FRICTION_ROLLING  = 0.0f;
    static const float DEFAULT_RESTITUTION       = 0.0f;
    static const float DEFAULT_DEACTIVATION_TIME = 2000;

    class MotionState : public btMotionState
    {
    public:
        MotionState(PhysicsBody* rigidBody) { m_rigidBody = rigidBody; }

        // Update from engine, ENGINE -> BULLET
        void getWorldTransform(btTransform& worldTrans) const override
        {
            const Vector3 lastPos    = m_rigidBody->GetTransform()->GetPosition();
            const Quaternion lastRot = m_rigidBody->GetTransform()->GetRotation();

            worldTrans.setOrigin(ToBtVector3(lastPos + lastRot * m_rigidBody->GetCenterOfMass()));
            worldTrans.setRotation(ToBtQuaternion(lastRot));
        }

        // Update from bullet, BULLET -> ENGINE
        void setWorldTransform(const btTransform& worldTrans) override
        {
            const Quaternion newWorldRot = ToQuaternion(worldTrans.getRotation());
            const Vector3 newWorldPos    = ToVector3(worldTrans.getOrigin()) - newWorldRot * m_rigidBody->GetCenterOfMass();

            m_rigidBody->GetTransform()->SetPosition(newWorldPos);
            m_rigidBody->GetTransform()->SetRotation(newWorldRot);
        }
    private:
        PhysicsBody* m_rigidBody;
    };

    PhysicsBody::PhysicsBody(weak_ptr<Entity> entity) : Component(entity)
    {
        m_in_world         = false;
        m_mass             = DEFAULT_MASS;
        m_restitution      = DEFAULT_RESTITUTION;
        m_friction         = DEFAULT_FRICTION;
        m_friction_rolling = DEFAULT_FRICTION_ROLLING;
        m_use_gravity      = true;
        m_gravity          = Physics::GetGravity();
        m_is_kinematic     = false;
        m_position_lock    = Vector3::Zero;
        m_rotation_lock    = Vector3::Zero;
        m_collision_shape  = nullptr;
        m_rigid_body       = nullptr;

        SP_REGISTER_ATTRIBUTE_VALUE_VALUE(m_mass, float);
        SP_REGISTER_ATTRIBUTE_VALUE_VALUE(m_friction, float);
        SP_REGISTER_ATTRIBUTE_VALUE_VALUE(m_friction_rolling, float);
        SP_REGISTER_ATTRIBUTE_VALUE_VALUE(m_restitution, float);
        SP_REGISTER_ATTRIBUTE_VALUE_VALUE(m_use_gravity, bool);
        SP_REGISTER_ATTRIBUTE_VALUE_VALUE(m_is_kinematic, bool);
        SP_REGISTER_ATTRIBUTE_VALUE_VALUE(m_gravity, Vector3);
        SP_REGISTER_ATTRIBUTE_VALUE_VALUE(m_position_lock, Vector3);
        SP_REGISTER_ATTRIBUTE_VALUE_VALUE(m_rotation_lock, Vector3);
        SP_REGISTER_ATTRIBUTE_VALUE_VALUE(m_center_of_mass, Vector3);
    }

    PhysicsBody::~PhysicsBody()
    {
        Body_Release();
    }

    void PhysicsBody::OnInitialize()
    {
        Component::OnInitialize();

        Body_AcquireShape();
        Body_AddToWorld();
    }

    void PhysicsBody::OnRemove()
    {
        Body_Release();
    }

    void PhysicsBody::OnStart()
    {
        Activate();
    }

    void PhysicsBody::OnTick()
    {
        // When the rigid body is inactive or we are in editor mode, allow the user to move/rotate it
        if (!IsActivated() || !Engine::IsFlagSet(EngineMode::Game))
        {
            if (GetPosition() != GetTransform()->GetPosition())
            {
                SetPosition(GetTransform()->GetPosition(), false);
                SetLinearVelocity(Vector3::Zero, false);
                SetAngularVelocity(Vector3::Zero, false);
            }

            if (GetRotation() != GetTransform()->GetRotation())
            {
                SetRotation(GetTransform()->GetRotation(), false);
                SetLinearVelocity(Vector3::Zero, false);
                SetAngularVelocity(Vector3::Zero, false);
            }
        }
    }

    void PhysicsBody::Serialize(FileStream* stream)
    {
        stream->Write(m_mass);
        stream->Write(m_friction);
        stream->Write(m_friction_rolling);
        stream->Write(m_restitution);
        stream->Write(m_use_gravity);
        stream->Write(m_gravity);
        stream->Write(m_is_kinematic);
        stream->Write(m_position_lock);
        stream->Write(m_rotation_lock);
        stream->Write(m_in_world);
    }

    void PhysicsBody::Deserialize(FileStream* stream)
    {
        stream->Read(&m_mass);
        stream->Read(&m_friction);
        stream->Read(&m_friction_rolling);
        stream->Read(&m_restitution);
        stream->Read(&m_use_gravity);
        stream->Read(&m_gravity);
        stream->Read(&m_is_kinematic);
        stream->Read(&m_position_lock);
        stream->Read(&m_rotation_lock);
        stream->Read(&m_in_world);

        Body_AcquireShape();
        Body_AddToWorld();
    }

    void PhysicsBody::SetMass(float mass)
    {
        mass = Helper::Max(mass, 0.0f);
        if (mass != m_mass)
        {
            m_mass = mass;
            Body_AddToWorld();
        }
    }

    void PhysicsBody::SetFriction(float friction)
    {
        if (!m_rigid_body || m_friction == friction)
            return;

        m_friction = friction;
        m_rigid_body->setFriction(friction);
    }

    void PhysicsBody::SetFrictionRolling(float frictionRolling)
    {
        if (!m_rigid_body || m_friction_rolling == frictionRolling)
            return;

        m_friction_rolling = frictionRolling;
        m_rigid_body->setRollingFriction(frictionRolling);
    }

    void PhysicsBody::SetRestitution(float restitution)
    {
        if (!m_rigid_body || m_restitution == restitution)
            return;

        m_restitution = restitution;
        m_rigid_body->setRestitution(restitution);
    }

    void PhysicsBody::SetUseGravity(bool gravity)
    {
        if (gravity == m_use_gravity)
            return;

        m_use_gravity = gravity;
        Body_AddToWorld();
    }

    void PhysicsBody::SetGravity(const Vector3& acceleration)
    {
        if (m_gravity == acceleration)
            return;

        m_gravity = acceleration;
        Body_AddToWorld();
    }

    void PhysicsBody::SetIsKinematic(bool kinematic)
    {
        if (kinematic == m_is_kinematic)
            return;

        m_is_kinematic = kinematic;
        Body_AddToWorld();
    }

    void PhysicsBody::SetLinearVelocity(const Vector3& velocity, const bool activate /*= true*/) const
    {
        if (!m_rigid_body)
            return;

        m_rigid_body->setLinearVelocity(ToBtVector3(velocity));
        if (velocity != Vector3::Zero && activate)
        {
            Activate();
        }
    }

    void PhysicsBody::SetAngularVelocity(const Vector3& velocity, const bool activate /*= true*/) const
    {
        if (!m_rigid_body)
            return;

        m_rigid_body->setAngularVelocity(ToBtVector3(velocity));
        if (velocity != Vector3::Zero && activate)
        {
            Activate();
        }
    }

    void PhysicsBody::ApplyForce(const Vector3& force, ForceMode mode) const
    {
        if (!m_rigid_body)
            return;

        Activate();

        if (mode == Force)
        {
            m_rigid_body->applyCentralForce(ToBtVector3(force));
        }
        else if (mode == Impulse)
        {
            m_rigid_body->applyCentralImpulse(ToBtVector3(force));
        }
    }

    void PhysicsBody::ApplyForceAtPosition(const Vector3& force, const Vector3& position, ForceMode mode) const
    {
        if (!m_rigid_body)
            return;

        Activate();

        if (mode == Force)
        {
            m_rigid_body->applyForce(ToBtVector3(force), ToBtVector3(position));
        }
        else if (mode == Impulse)
        {
            m_rigid_body->applyImpulse(ToBtVector3(force), ToBtVector3(position));
        }
    }

    void PhysicsBody::ApplyTorque(const Vector3& torque, ForceMode mode) const
    {
        if (!m_rigid_body)
            return;

        Activate();

        if (mode == Force)
        {
            m_rigid_body->applyTorque(ToBtVector3(torque));
        }
        else if (mode == Impulse)
        {
            m_rigid_body->applyTorqueImpulse(ToBtVector3(torque));
        }
    }

    void PhysicsBody::SetPositionLock(bool lock)
    {
        if (lock)
        {
            SetPositionLock(Vector3::One);
        }
        else
        {
            SetPositionLock(Vector3::Zero);
        }
    }

    void PhysicsBody::SetPositionLock(const Vector3& lock)
    {
        if (!m_rigid_body || m_position_lock == lock)
            return;

        m_position_lock = lock;
        m_rigid_body->setLinearFactor(ToBtVector3(Vector3::One - lock));
    }

    void PhysicsBody::SetRotationLock(bool lock)
    {
        if (lock)
        {
            SetRotationLock(Vector3::One);
        }
        else
        {
            SetRotationLock(Vector3::Zero);
        }
    }

    void PhysicsBody::SetRotationLock(const Vector3& lock)
    {
        if (!m_rigid_body || m_rotation_lock == lock)
            return;

        m_rotation_lock = lock;
        m_rigid_body->setAngularFactor(ToBtVector3(Vector3::One - lock));
    }

    void PhysicsBody::SetCenterOfMass(const Vector3& centerOfMass)
    {
        m_center_of_mass = centerOfMass;
        SetPosition(GetPosition());
    }

    Vector3 PhysicsBody::GetPosition() const
    {
        if (m_rigid_body)
        {
            const btTransform& transform = m_rigid_body->getWorldTransform();
            return ToVector3(transform.getOrigin()) - ToQuaternion(transform.getRotation()) * m_center_of_mass;
        }
    
        return Vector3::Zero;
    }

    void PhysicsBody::SetPosition(const Vector3& position, const bool activate /*= true*/) const
    {
        if (!m_rigid_body)
            return;

        // Set position to world transform
        btTransform& transform_world = m_rigid_body->getWorldTransform();
        transform_world.setOrigin(ToBtVector3(position + ToQuaternion(transform_world.getRotation()) * m_center_of_mass));

        // Set position to interpolated world transform
        btTransform transform_world_interpolated = m_rigid_body->getInterpolationWorldTransform();
        transform_world_interpolated.setOrigin(transform_world.getOrigin());
        m_rigid_body->setInterpolationWorldTransform(transform_world_interpolated);

        if (activate)
        {
            Activate();
        }
    }

    Quaternion PhysicsBody::GetRotation() const
    {
        return m_rigid_body ? ToQuaternion(m_rigid_body->getWorldTransform().getRotation()) : Quaternion::Identity;
    }

    void PhysicsBody::SetRotation(const Quaternion& rotation, const bool activate /*= true*/) const
    {
        if (!m_rigid_body)
            return;

        // Set rotation to world transform
        const Vector3 oldPosition = GetPosition();
        btTransform& transform_world = m_rigid_body->getWorldTransform();
        transform_world.setRotation(ToBtQuaternion(rotation));
        if (m_center_of_mass != Vector3::Zero)
        {
            transform_world.setOrigin(ToBtVector3(oldPosition + rotation * m_center_of_mass));
        }

        // Set rotation to interpolated world transform
        btTransform interpTrans = m_rigid_body->getInterpolationWorldTransform();
        interpTrans.setRotation(transform_world.getRotation());
        if (m_center_of_mass != Vector3::Zero)
        {
            interpTrans.setOrigin(transform_world.getOrigin());
        }
        m_rigid_body->setInterpolationWorldTransform(interpTrans);

        m_rigid_body->updateInertiaTensor();

        if (activate)
        {
            Activate();
        }
    }

    void PhysicsBody::ClearForces() const
    {
        if (!m_rigid_body)
            return;

        m_rigid_body->clearForces();
    }

    void PhysicsBody::Activate() const
    {
        if (!m_rigid_body)
            return;

        if (m_mass > 0.0f)
        {
            m_rigid_body->activate(true);
        }
    }

    void PhysicsBody::Deactivate() const
    {
        if (!m_rigid_body)
            return;

        m_rigid_body->setActivationState(WANTS_DEACTIVATION);
    }

    bool PhysicsBody::IsActive() const
    {
        return m_rigid_body->isActive();
    }

    void PhysicsBody::AddConstraint(Constraint* constraint)
    {
        m_constraints.emplace_back(constraint);
    }

    void PhysicsBody::RemoveConstraint(Constraint* constraint)
    {
        for (auto it = m_constraints.begin(); it != m_constraints.end(); )
        {
            const auto itConstraint = *it;
            if (constraint->GetObjectId() == itConstraint->GetObjectId())
            {
                it = m_constraints.erase(it);
            }
            else
            {
                ++it;
            }
        }

        Activate();
    }

    void PhysicsBody::SetShape(btCollisionShape* shape)
    {
        m_collision_shape = shape;

        if (m_collision_shape)
        {
            Body_AddToWorld();
        }
        else
        {
            Body_RemoveFromWorld();
        }
    }

    void PhysicsBody::Body_AddToWorld()
    {
        if (m_mass < 0.0f)
        {
            m_mass = 0.0f;
        }

        // Transfer inertia to new collision shape
        btVector3 local_intertia = btVector3(0, 0, 0);
        if (m_collision_shape && m_rigid_body)
        {
            local_intertia = m_rigid_body ? m_rigid_body->getLocalInertia() : local_intertia;
            m_collision_shape->calculateLocalInertia(m_mass, local_intertia);
        }
        
        Body_Release();

        // CONSTRUCTION
        {
            // Create a motion state (memory will be freed by the RigidBody)
            const auto motion_state = new MotionState(this);
            
            // Info
            btRigidBody::btRigidBodyConstructionInfo constructionInfo(m_mass, motion_state, m_collision_shape, local_intertia);
            constructionInfo.m_mass            = m_mass;
            constructionInfo.m_friction        = m_friction;
            constructionInfo.m_rollingFriction = m_friction_rolling;
            constructionInfo.m_restitution     = m_restitution;
            constructionInfo.m_collisionShape  = m_collision_shape;
            constructionInfo.m_localInertia    = local_intertia;
            constructionInfo.m_motionState     = motion_state;

            m_rigid_body = new btRigidBody(constructionInfo);
            m_rigid_body->setUserPointer(this);
        }

        // Reapply constraint positions for new center of mass shift
        for (const auto& constraint : m_constraints)
        {
            constraint->ApplyFrames();
        }
        
        Flags_UpdateKinematic();
        Flags_UpdateGravity();

        // Transform
        SetPosition(GetTransform()->GetPosition());
        SetRotation(GetTransform()->GetRotation());

        // Constraints
        SetPositionLock(m_position_lock);
        SetRotationLock(m_rotation_lock);

        // Position and rotation locks
        SetPositionLock(m_position_lock);
        SetRotationLock(m_rotation_lock);

        // Add to world
        Physics::AddBody(m_rigid_body);

        if (m_mass > 0.0f)
        {
            Activate();
        }
        else
        {
            SetLinearVelocity(Vector3::Zero);
            SetAngularVelocity(Vector3::Zero);
        }

        m_in_world = true;
    }

    void PhysicsBody::Body_Release()
    {
        if (!m_rigid_body)
            return;

        // Release any constraints that refer to it
        for (const auto& constraint : m_constraints)
        {
            constraint->ReleaseConstraint();
        }

        // Remove it from the world
        Body_RemoveFromWorld();

        // Reset it
        m_rigid_body = nullptr;
    }

    void PhysicsBody::Body_RemoveFromWorld()
    {
        if (!m_rigid_body)
            return;

        if (m_in_world)
        {
            Physics::RemoveBody(m_rigid_body);
            m_in_world = false;
        }
    }

    void PhysicsBody::Body_AcquireShape()
    {
        if (const auto& collider = m_entity_ptr->GetComponent<Collider>())
        {
            m_collision_shape    = collider->GetShape();
            m_center_of_mass    = collider->GetCenter();
        }
    }

    void PhysicsBody::Flags_UpdateKinematic() const
    {
        int flags = m_rigid_body->getCollisionFlags();

        if (m_is_kinematic)
        {
            flags |= btCollisionObject::CF_KINEMATIC_OBJECT;
        }
        else
        {
            flags &= ~btCollisionObject::CF_KINEMATIC_OBJECT;
        }

        m_rigid_body->setCollisionFlags(flags);
        m_rigid_body->forceActivationState(m_is_kinematic ? DISABLE_DEACTIVATION : ISLAND_SLEEPING);
        m_rigid_body->setDeactivationTime(DEFAULT_DEACTIVATION_TIME);
    }

    void PhysicsBody::Flags_UpdateGravity() const
    {
        int flags = m_rigid_body->getFlags();

        if (m_use_gravity)
        {
            flags &= ~BT_DISABLE_WORLD_GRAVITY;
        }
        else
        {
            flags |= BT_DISABLE_WORLD_GRAVITY;
        }

        m_rigid_body->setFlags(flags);

        if (m_use_gravity)
        {
            m_rigid_body->setGravity(ToBtVector3(m_gravity));
        }
        else
        {
            m_rigid_body->setGravity(btVector3(0.0f, 0.0f, 0.0f));
        }
    }

    bool PhysicsBody::IsActivated() const
    {
        return m_rigid_body->isActive();
    }
}