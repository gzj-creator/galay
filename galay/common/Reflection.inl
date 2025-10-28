#ifndef GALAY_REFLECION_INL
#define GALAY_REFLECION_INL

template <typename... Targs>
common::RequestFactory<Targs...>* common::RequestFactory<Targs...>::m_ReqFactory = nullptr;

template <typename... Targs>
common::RequestFactory<Targs...>* 
common::RequestFactory<Targs...>::getInstance()
{
    if (m_ReqFactory == nullptr)
    {
        m_ReqFactory =  new RequestFactory;
        FactoryManager::addReleaseFunc([](){
            if(m_ReqFactory){
                delete m_ReqFactory;
                m_ReqFactory = nullptr;
            }
        });
    }
    return m_ReqFactory;
}

template <typename... Targs>
bool 
common::RequestFactory<Targs...>::regist(const ::std::string &typeName, ::std::function<::std::shared_ptr<Request>(Targs &&...args)> func)
{
    if (nullptr == func)
        return false;
    m_mapCreateFunction.insert(::std::make_pair(typeName, func));
    return true;
}

template <typename... Targs>
::std::shared_ptr<Request>
common::RequestFactory<Targs...>::create(const ::std::string &typeName, Targs &&...args)
{
    if (m_mapCreateFunction.contains(typeName))
    {
        return m_mapCreateFunction[typeName](::std::forward<Targs>(args)...);
    }
    return nullptr;
}


//response factory
template <typename... Targs>
common::ResponseFactory<Targs...> *common::ResponseFactory<Targs...>::m_RespFactory = nullptr;


template <typename... Targs>
common::ResponseFactory<Targs...>*
common::ResponseFactory<Targs...>::getInstance()
{
    if (m_RespFactory == nullptr)
    {
        m_RespFactory = new ResponseFactory ;
        FactoryManager::addReleaseFunc([](){
            if(m_RespFactory){
                delete m_RespFactory;
                m_RespFactory = nullptr;
            }
        });
    }
    return m_RespFactory;
}

template <typename... Targs>
bool 
common::ResponseFactory<Targs...>::regist(const ::std::string &typeName, ::std::function<::std::shared_ptr<Response>(Targs &&...args)> func)
{
    if (nullptr == func)
        return false;
    m_mapCreateFunction.insert(::std::make_pair(typeName, func));
    return true;
}

template <typename... Targs>
::std::shared_ptr<Response>
common::ResponseFactory<Targs...>::create(const ::std::string &typeName, Targs &&...args)
{
    if (m_mapCreateFunction.contains(typeName))
    {
        return m_mapCreateFunction[typeName](::std::forward<Targs>(args)...);
    }
    return nullptr;
}


//user factory
template <typename... Targs>
common::UserFactory<Targs...> *common::UserFactory<Targs...>::m_userFactory = nullptr;


template <typename... Targs>
common::UserFactory<Targs...>*
common::UserFactory<Targs...>::getInstance()
{
    if (m_userFactory == nullptr)
    {
        m_userFactory = new UserFactory;
        FactoryManager::addReleaseFunc([](){
            if(m_userFactory){
                delete m_userFactory;
                m_userFactory = nullptr;
            }
        });
    }
    return m_userFactory;
}


template <typename... Targs>
bool 
common::UserFactory<Targs...>::regist(const ::std::string &typeName, ::std::function<::std::shared_ptr<Base>(Targs &&...args)> func)
{
    if (nullptr == func)
        return false;
    m_mapCreateFunction.insert(::std::make_pair(typeName, func));
    return true;
}

template <typename... Targs>
::std::shared_ptr<Base>
common::UserFactory<Targs...>::create(const ::std::string &typeName, Targs &&...args)
{
    if (m_mapCreateFunction.contains(typeName))
    {
        return m_mapCreateFunction[typeName](::std::forward<Targs>(args)...);
    }
    return nullptr;
}

//DynamicCreator

template <typename BaseClass, typename T, typename... Targs>
typename common::Register<BaseClass,T, Targs...> common::DynamicCreator<BaseClass, T, Targs...>::m_register;

template <typename BaseClass, typename T, typename... Targs>
typename ::std::string common::DynamicCreator<BaseClass, T, Targs...>::m_typeName;

template <typename BaseClass, typename T, typename... Targs>
common::DynamicCreator<BaseClass, T, Targs...>::DynamicCreator()
{
    m_register.do_nothing();
}

template <typename BaseClass, typename T, typename... Targs>
::std::shared_ptr<T> 
common::DynamicCreator<BaseClass, T, Targs...>::createObject(Targs &&...args)
{
    return ::std::make_shared<T>(::std::forward<Targs>(args)...);
}

template <typename BaseClass, typename T, typename... Targs>
const ::std::string 
common::DynamicCreator<BaseClass, T, Targs...>::getTypeName()
{
    return m_typeName;
}

template <typename BaseClass, typename T, typename... Targs>
common::DynamicCreator<BaseClass, T, Targs...>::~DynamicCreator()
{
    m_register.do_nothing();
}


// Register

 template <typename BaseClass, typename T, typename... Targs>
 common::Register<BaseClass,T,Targs...>::Register()
{
    ::std::string typeName = utils::getTypeName<T>();
}

template <typename T, typename... Targs>
common::Register<Request,T,Targs...>::Register()
{
    ::std::string typeName = utils::getTypeName<T>();
    common::RequestFactory<Targs...>::getInstance()->regist(typeName, common::DynamicCreator<Request, T, Targs...>::createObject);
}

template <typename T, typename... Targs>
common::Register<Response,T,Targs...>::Register()
{
    ::std::string typeName = utils::getTypeName<T>();
    common::ResponseFactory<Targs...>::getInstance()->regist(typeName, common::DynamicCreator<Response, T, Targs...>::createObject);
}

template <typename T, typename... Targs>
common::Register<Base,T,Targs...>::Register()
{
    ::std::string typeName = utils::getTypeName<T>();
    common::UserFactory<Targs...>::getInstance()->regist(typeName, common::DynamicCreator<Base, T, Targs...>::createObject);
}

#endif
